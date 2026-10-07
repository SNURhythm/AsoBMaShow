package com.snurhythm.asobmashow;

import java.io.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

public final class ImportCopyWorkersTests {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void deviceBudget(int processors, int expected) throws Exception {
        CountDownLatch started = new CountDownLatch(expected);
        CountDownLatch extra = new CountDownLatch(expected + 1);
        CountDownLatch release = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread coordinator = new Thread(() -> {
            try (ImportCopyWorkers workers = new ImportCopyWorkers(processors, () -> {}, () -> {})) {
                for (int i = 0; i < 20; i++) workers.submit(() -> {
                    started.countDown(); extra.countDown();
                    try { release.await(); }
                    catch (InterruptedException error) { throw new InterruptedIOException(); }
                });
                workers.awaitAll();
            } catch (Throwable error) { failure.set(error); }
        });
        coordinator.start();
        try {
            require(started.await(2, TimeUnit.SECONDS), "Did not use the device's available parallelism");
            require(!extra.await(100, TimeUnit.MILLISECONDS), "Exceeded the device or I/O worker budget");
        } finally {
            release.countDown();
            coordinator.join(3000);
        }
        require(!coordinator.isAlive() && failure.get() == null, "Copy workers did not finish cleanly");
    }

    private static void cancellationClosesBlockedReadersBeforeReturning() throws Exception {
        CountDownLatch reading = new CountDownLatch(2);
        CountDownLatch closed = new CountDownLatch(2);
        AtomicBoolean cancelled = new AtomicBoolean();
        AtomicBoolean providerCancelled = new AtomicBoolean();
        AtomicBoolean returned = new AtomicBoolean();
        Thread coordinator = new Thread(() -> {
            try (ImportCopyWorkers workers = new ImportCopyWorkers(2, () -> {
                if (cancelled.get()) throw new InterruptedIOException();
            }, () -> providerCancelled.set(true))) {
                for (int i = 0; i < 2; i++) workers.submit(() -> {
                    try (InputStream input = workers.track(new InputStream() {
                        private boolean done;
                        public synchronized int read() throws IOException {
                            reading.countDown();
                            while (!done) {
                                try { wait(); }
                                catch (InterruptedException ignored) { } // Provider needs close, not interrupt.
                            }
                            throw new IOException("closed");
                        }
                        public synchronized void close() { done = true; closed.countDown(); notifyAll(); }
                    })) { input.read(); }
                });
                workers.awaitAll();
            } catch (IOException expected) { returned.set(true); }
        });
        coordinator.start();
        try {
            require(reading.await(2, TimeUnit.SECONDS), "Readers never started");
            cancelled.set(true);
            coordinator.join(3000);
            require(!coordinator.isAlive() && returned.get() && providerCancelled.get(), "Cancellation stuck");
            require(closed.getCount() == 0, "Returned while source streams were still open");
        } finally { cancelled.set(true); coordinator.interrupt(); coordinator.join(3000); }
    }

    private static void failedCopyDrainsOtherReadersAndPreservesError() throws Exception {
        CountDownLatch reading = new CountDownLatch(1);
        AtomicBoolean closed = new AtomicBoolean();
        AtomicBoolean aborted = new AtomicBoolean();
        try (ImportCopyWorkers workers = new ImportCopyWorkers(2, () -> {}, () -> aborted.set(true))) {
            workers.submit(() -> {
                try (InputStream input = workers.track(new InputStream() {
                    public synchronized int read() {
                        reading.countDown();
                        while (!closed.get()) {
                            try { wait(); } catch (InterruptedException ignored) { }
                        }
                        return -1;
                    }
                    public synchronized void close() { closed.set(true); notifyAll(); }
                })) { input.read(); }
            });
            workers.submit(() -> {
                try { require(reading.await(2, TimeUnit.SECONDS), "Sibling read never started"); }
                catch (InterruptedException error) { throw new InterruptedIOException(); }
                throw new IOException("fixture write failed");
            });
            workers.awaitAll();
            throw new AssertionError("Copy failure was swallowed");
        } catch (IOException error) {
            require(error.getMessage().equals("fixture write failed"), "Original error was replaced");
            require(closed.get() && aborted.get(), "Failure returned before sibling cleanup");
        }
    }

    private static void monitorFailureAbortsEvenWithoutCopies() throws Exception {
        CountDownLatch cancelled = new CountDownLatch(1);
        try (ImportCopyWorkers workers = new ImportCopyWorkers(() -> {}, () -> {
            throw new IOException("Control state unavailable");
        }, cancelled::countDown)) {
            require(cancelled.await(2, TimeUnit.SECONDS), "Monitor failure never cancelled pending provider I/O");
            workers.awaitAll();
            throw new AssertionError("Monitor failure must fail an empty transfer");
        } catch (IOException expected) {
            require(expected.getMessage().equals("Control state unavailable"), "Monitor failure lost its original cause");
        }
    }

    private static void normalCloseStopsCancellationMonitor() throws Exception {
        CountDownLatch checked = new CountDownLatch(1);
        AtomicReference<Thread> monitor = new AtomicReference<>();
        AtomicBoolean providerCancelled = new AtomicBoolean();
        try (ImportCopyWorkers workers = new ImportCopyWorkers(() -> {}, () -> {
            monitor.set(Thread.currentThread());
            checked.countDown();
        }, () -> providerCancelled.set(true))) {
            require(checked.await(2, TimeUnit.SECONDS), "Cancellation monitor never started");
        }
        require(!monitor.get().isAlive() && !providerCancelled.get(),
                "Successful close must join monitor without cancelling the provider");
    }

    public static void main(String[] args) throws Exception {
        monitorFailureAbortsEvenWithoutCopies();
        normalCloseStopsCancellationMonitor();
        deviceBudget(1, 1);
        deviceBudget(2, 2);
        deviceBudget(0, 4);
        deviceBudget(32, 8);
        cancellationClosesBlockedReadersBeforeReturning();
        failedCopyDrainsOtherReadersAndPreservesError();
        System.out.println("8 import copy worker tests passed");
    }
}
