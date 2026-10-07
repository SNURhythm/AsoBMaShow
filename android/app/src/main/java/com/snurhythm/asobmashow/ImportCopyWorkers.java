package com.snurhythm.asobmashow;

import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.Set;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;

/** Bounded independent file copies. Closing drains workers before callers remove staging. */
final class ImportCopyWorkers implements AutoCloseable {
    interface Action { void run() throws IOException; }
    private final int workerCount;
    private final ExecutorService executor;
    private final CompletionService<Void> completed;
    private final ThreadLocal<byte[]> buffers = ThreadLocal.withInitial(() -> new byte[1024 * 1024]);
    private final Set<InputStream> inputs = Collections.newSetFromMap(new IdentityHashMap<>());
    private final Action checkpoint;
    private final Runnable cancelPendingIo;
    private final Thread cancellationMonitor;
    private final AtomicBoolean closed = new AtomicBoolean();
    private final AtomicBoolean providerCancelled = new AtomicBoolean();
    private volatile IOException cancellationFailure;
    private int pending;
    private boolean stopping;

    ImportCopyWorkers(Action checkpoint, Runnable cancelPendingIo) {
        this(Runtime.getRuntime().availableProcessors(), checkpoint, cancelPendingIo);
    }

    ImportCopyWorkers(Action checkpoint, Action checkCancellation, Runnable cancelPendingIo) {
        this(Runtime.getRuntime().availableProcessors(), checkpoint, checkCancellation, cancelPendingIo);
    }

    ImportCopyWorkers(int availableProcessors, Action checkpoint, Runnable cancelPendingIo) {
        this(availableProcessors, checkpoint, checkpoint, cancelPendingIo);
    }

    private ImportCopyWorkers(int availableProcessors, Action checkpoint, Action checkCancellation,
                              Runnable cancelPendingIo) {
        // Like chart parsing, use reported hardware parallelism (fallback: four).
        // File I/O also has an eight-stream / eight-MiB buffer ceiling.
        workerCount = Math.min(8, availableProcessors > 0 ? availableProcessors : 4);
        executor = Executors.newFixedThreadPool(workerCount);
        completed = new ExecutorCompletionService<>(executor);
        this.checkpoint = () -> {
            if (cancellationFailure != null) throw cancellationFailure;
            checkpoint.run();
        };
        this.cancelPendingIo = cancelPendingIo;
        Thread coordinator = Thread.currentThread();
        cancellationMonitor = new Thread(() -> {
            try {
                while (!closed.get()) {
                    if (coordinator.isInterrupted()) throw cancelled();
                    checkCancellation.run();
                    Thread.sleep(50);
                }
            } catch (IOException | RuntimeException | InterruptedException error) {
                if (!closed.get()) {
                    cancellationFailure = error instanceof IOException ? (IOException)error
                            : new IOException("Import copy cancelled.", error);
                    stopIo(true);
                }
            }
        }, "import-cancellation");
        cancellationMonitor.start();
    }

    void submit(Action action) throws IOException {
        checkpoint.run();
        while (pending >= workerCount * 2) awaitOne();
        completed.submit(() -> {
            if (Thread.currentThread().isInterrupted()) throw cancelled();
            action.run();
            return null;
        });
        pending++;
    }

    byte[] buffer() { return buffers.get(); }

    InputStream track(InputStream input) throws IOException {
        if (input == null) throw new IOException("Could not read import file.");
        InputStream tracked = new FilterInputStream(input) {
            private final AtomicBoolean closed = new AtomicBoolean();
            @Override public void close() throws IOException {
                if (!closed.compareAndSet(false, true)) return;
                try { super.close(); }
                finally { synchronized (ImportCopyWorkers.this) { inputs.remove(this); } }
            }
        };
        synchronized (this) {
            if (!stopping) {
                inputs.add(tracked);
                return tracked;
            }
        }
        tracked.close();
        throw cancelled();
    }

    void awaitAll() throws IOException {
        checkpoint.run();
        while (pending > 0) awaitOne();
    }

    private void awaitOne() throws IOException {
        try {
            while (true) {
                checkpoint.run();
                Future<Void> future = completed.poll(50, TimeUnit.MILLISECONDS);
                if (future == null) continue;
                pending--;
                future.get();
                return;
            }
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw cancelled();
        } catch (ExecutionException error) {
            Throwable cause = error.getCause();
            if (cause instanceof IOException) throw (IOException)cause;
            if (cause instanceof RuntimeException) throw (RuntimeException)cause;
            throw new IOException("Import copy failed.", cause);
        }
    }

    private void stopIo(boolean cancelProvider) {
        InputStream[] remaining;
        synchronized (this) {
            stopping = true;
            remaining = inputs.toArray(new InputStream[0]);
        }
        executor.shutdownNow();
        if (cancelProvider && providerCancelled.compareAndSet(false, true)) {
            try { cancelPendingIo.run(); } catch (RuntimeException ignored) { }
        }
        for (InputStream input : remaining) {
            try { input.close(); } catch (IOException | RuntimeException ignored) { }
        }
    }

    @Override public void close() {
        closed.set(true);
        cancellationMonitor.interrupt();
        stopIo(pending > 0);
        // A provider stream may need close/cancellation as well as interruption.
        // Never let output cleanup race a still-running writer.
        boolean interrupted = Thread.interrupted();
        while (cancellationMonitor.isAlive()) {
            try { cancellationMonitor.join(50); }
            catch (InterruptedException error) { interrupted = true; }
        }
        while (!executor.isTerminated()) {
            try { executor.awaitTermination(50, TimeUnit.MILLISECONDS); }
            catch (InterruptedException error) { interrupted = true; }
        }
        if (interrupted) Thread.currentThread().interrupt();
    }

    private static InterruptedIOException cancelled() {
        return new InterruptedIOException("Import copy cancelled.");
    }
}
