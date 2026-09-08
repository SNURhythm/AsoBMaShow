package com.snurhythm.asobmashow;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.InterruptedIOException;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

public final class ChartImportCopyControlTests {
    private static void require(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    private static void await(CountDownLatch latch, String message) throws Exception {
        require(latch.await(2, TimeUnit.SECONDS), message);
    }

    private static void finish(Thread worker) throws Exception {
        worker.join(2000);
        if (worker.isAlive()) {
            worker.interrupt();
            worker.join(2000);
            throw new AssertionError("Copy worker did not finish");
        }
    }

    private static void testPauseBeforeReadingAndResume() throws Exception {
        AtomicInteger state = new AtomicInteger(0);
        AtomicInteger reads = new AtomicInteger();
        CountDownLatch paused = new CountDownLatch(1);
        byte[] bytes = {1, 2, 3};
        ByteArrayInputStream input = new ByteArrayInputStream(bytes) {
            @Override
            public synchronized int read(byte[] buffer, int offset, int length) {
                reads.incrementAndGet();
                return super.read(buffer, offset, length);
            }
        };
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        ChartImportCopyControl control = new ChartImportCopyControl(() -> {
            if (state.get() == 0) {
                paused.countDown();
            }
            return state.get();
        }, () -> false);
        AtomicReference<Exception> failure = new AtomicReference<>();
        Thread worker = new Thread(() -> {
            try {
                control.copy(input, output);
            } catch (Exception error) {
                failure.set(error);
            }
        });
        worker.start();
        await(paused, "Copy must reach the native pause checkpoint");
        require(reads.get() == 0 && output.size() == 0,
                "Paused copy must not read or write storage");
        state.set(1);
        finish(worker);
        require(failure.get() == null && Arrays.equals(bytes, output.toByteArray()),
                "Resume must complete the original stream without restarting");
    }

    private static void testPauseBetweenReadAndWrite() throws Exception {
        AtomicInteger state = new AtomicInteger(1);
        CountDownLatch paused = new CountDownLatch(1);
        ByteArrayInputStream input = new ByteArrayInputStream(new byte[] {4, 5}) {
            @Override
            public synchronized int read(byte[] buffer, int offset, int length) {
                int count = super.read(buffer, offset, length);
                if (count > 0) {
                    state.set(0);
                }
                return count;
            }
        };
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        ChartImportCopyControl control = new ChartImportCopyControl(() -> {
            if (state.get() == 0) {
                paused.countDown();
            }
            return state.get();
        }, () -> false);
        AtomicReference<Exception> failure = new AtomicReference<>();
        Thread worker = new Thread(() -> {
            try {
                control.copy(input, output);
            } catch (Exception error) {
                failure.set(error);
            }
        });
        worker.start();
        await(paused, "A pause during read must be checked before writing");
        require(output.size() == 0, "A newly paused copy must not write its buffered chunk");
        state.set(-1);
        finish(worker);
        require(failure.get() instanceof InterruptedIOException && output.size() == 0,
                "Native cancellation must discard the buffered chunk");
    }

    private static void testActivityDestructionWakesPausedWorker() throws Exception {
        AtomicBoolean destroyed = new AtomicBoolean(false);
        CountDownLatch paused = new CountDownLatch(1);
        ChartImportCopyControl control = new ChartImportCopyControl(() -> {
            paused.countDown();
            return 0;
        }, destroyed::get);
        AtomicReference<Exception> failure = new AtomicReference<>();
        Thread worker = new Thread(() -> {
            try {
                control.checkpoint();
            } catch (Exception error) {
                failure.set(error);
            }
        });
        worker.start();
        await(paused, "Copy must wait off the UI thread");
        destroyed.set(true);
        worker.interrupt();
        finish(worker);
        require(failure.get() instanceof InterruptedIOException,
                "Destroy must cancel a paused copy without requiring gameplay resume");
        try {
            control.checkpoint();
            throw new AssertionError("Destroyed activity must reject new storage work");
        } catch (InterruptedIOException expected) {
        }
    }

    public static void main(String[] args) throws Exception {
        testPauseBeforeReadingAndResume();
        testPauseBetweenReadAndWrite();
        testActivityDestructionWakesPausedWorker();
        System.out.println("3 chart import copy control tests passed");
    }
}
