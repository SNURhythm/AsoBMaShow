package com.snurhythm.asobmashow;

import java.io.IOException;
import java.util.Objects;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;

/** Owns Android provider write leases through terminal rollback or completion. */
final class DocumentHandoffRollbackCoordinator {
    private static final int DEFAULT_QUEUE_CAPACITY = 8;

    @FunctionalInterface
    interface RestoreAction {
        boolean restore() throws Exception;
    }

    static final class Lease {
        private final DocumentHandoffRestoreRegistry.Ticket ticket;
        private final AtomicBoolean resolutionStarted = new AtomicBoolean(false);

        private Lease(DocumentHandoffRestoreRegistry.Ticket ticket) {
            this.ticket = ticket;
        }

        private boolean beginResolution() {
            return resolutionStarted.compareAndSet(false, true);
        }
    }

    private final DocumentHandoffRestoreRegistry registry;
    private final Executor executor;
    private final Consumer<Throwable> errorSink;

    static DocumentHandoffRollbackCoordinator createDefault(
            Consumer<Throwable> errorSink) {
        ThreadPoolExecutor executor = new ThreadPoolExecutor(
                1,
                1,
                0L,
                TimeUnit.MILLISECONDS,
                new ArrayBlockingQueue<>(DEFAULT_QUEUE_CAPACITY),
                runnable -> {
                    Thread thread = new Thread(
                            runnable, "AsoBMaShow-document-rollback");
                    thread.setDaemon(true);
                    return thread;
                },
                (task, ignored) -> task.run());
        return new DocumentHandoffRollbackCoordinator(
                new DocumentHandoffRestoreRegistry(), executor, errorSink);
    }

    DocumentHandoffRollbackCoordinator(
            DocumentHandoffRestoreRegistry registry,
            Executor executor,
            Consumer<Throwable> errorSink) {
        this.registry = Objects.requireNonNull(registry, "registry");
        this.executor = Objects.requireNonNull(executor, "executor");
        this.errorSink = Objects.requireNonNull(errorSink, "errorSink");
    }

    Lease acquire(String key, BooleanSupplier cancelled)
            throws InterruptedException {
        DocumentHandoffRestoreRegistry.Ticket ticket =
                registry.acquire(key, cancelled);
        return ticket == null ? null : new Lease(ticket);
    }

    void finish(Lease lease) {
        if (lease != null && lease.beginResolution()) {
            registry.complete(lease.ticket);
        }
    }

    void rollback(Lease lease, RestoreAction restoreAction) {
        if (lease == null) {
            return;
        }
        Objects.requireNonNull(restoreAction, "restoreAction");
        if (!lease.beginResolution()) {
            return;
        }

        AtomicBoolean attempted = new AtomicBoolean(false);
        Runnable task = () -> {
            if (!attempted.compareAndSet(false, true)) {
                return;
            }
            try {
                try {
                    if (!restoreAction.restore()) {
                        report(new IOException(
                                "The empty export destination could not be restored."));
                    }
                } catch (Exception failure) {
                    report(failure);
                }
            } finally {
                registry.complete(lease.ticket);
            }
        };

        try {
            executor.execute(task);
        } catch (RuntimeException ignored) {
            task.run();
        }
    }

    private void report(Throwable failure) {
        try {
            errorSink.accept(failure);
        } catch (RuntimeException ignored) {
            // Diagnostics must never strand a provider write lease.
        }
    }
}
