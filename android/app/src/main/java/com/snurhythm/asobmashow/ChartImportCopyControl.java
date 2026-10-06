package com.snurhythm.asobmashow;

import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.util.function.BooleanSupplier;
import java.util.function.IntSupplier;
import java.util.function.LongConsumer;
import java.util.function.LongSupplier;

final class ChartImportCopyControl {
    static final long MAXIMUM_ARCHIVE_BYTES = 8L * 1024 * 1024 * 1024;
    static final long ARCHIVE_RESERVED_BYTES = 256L * 1024 * 1024;
    private final IntSupplier copyState;
    private final BooleanSupplier cancelled;
    private long pauseGeneration;

    ChartImportCopyControl(IntSupplier copyState, BooleanSupplier cancelled) {
        this.copyState = copyState;
        this.cancelled = cancelled;
    }

    void checkpoint() throws InterruptedIOException {
        boolean waited = false;
        while (true) {
            int state = currentState();
            if (state > 0) {
                return;
            }
            if (!waited) {
                pauseGeneration++;
                waited = true;
            }
            try {
                Thread.sleep(20);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                throw new InterruptedIOException("Chart import cancelled.");
            }
        }
    }

    long pauseGeneration() {
        return pauseGeneration;
    }

    boolean continueWithoutWaiting(long validatedGeneration) throws InterruptedIOException {
        // Never suspend between validating a source snapshot and deleting it. If
        // validation (including a provider cursor) paused, the caller must list again.
        return currentState() > 0 && pauseGeneration == validatedGeneration;
    }

    private int currentState() throws InterruptedIOException {
        if (cancelled.getAsBoolean() || Thread.currentThread().isInterrupted()) {
            throw new InterruptedIOException("Chart import cancelled.");
        }
        int state = copyState.getAsInt();
        if (state < 0) throw new InterruptedIOException("Chart import cancelled.");
        return state;
    }

    void copy(InputStream input, OutputStream output) throws IOException {
        copy(input, output, null);
    }

    void copy(InputStream input, OutputStream output, LongConsumer bytesCopied) throws IOException {
        copy(input, output, Long.MAX_VALUE, 0, null, bytesCopied);
    }

    void copyArchive(InputStream input, OutputStream output, LongSupplier usableSpace)
            throws IOException {
        copyArchive(input, output, MAXIMUM_ARCHIVE_BYTES, ARCHIVE_RESERVED_BYTES, usableSpace);
    }

    void copyArchive(InputStream input, OutputStream output, long maximumBytes,
                     long reservedBytes, LongSupplier usableSpace) throws IOException {
        if (maximumBytes < 0 || reservedBytes < 0 || usableSpace == null) {
            throw new IllegalArgumentException("Invalid archive copy budget.");
        }
        copy(input, output, maximumBytes, reservedBytes, usableSpace, null);
    }

    private void copy(InputStream input, OutputStream output, long maximumBytes,
                      long reservedBytes, LongSupplier usableSpace, LongConsumer bytesCopied) throws IOException {
        byte[] buffer = new byte[1024 * 1024];
        long copiedBytes = 0;
        while (true) {
            checkpoint();
            int count = input.read(buffer);
            if (count < 0) {
                return;
            }
            checkpoint();
            if (count > maximumBytes - copiedBytes) {
                throw new IOException("Archive import exceeds the byte limit (" + maximumBytes + " bytes).");
            }
            if (usableSpace != null) {
                long available = usableSpace.getAsLong();
                if (available < reservedBytes || count > available - reservedBytes) {
                    throw new IOException("Not enough free space to import archive while preserving storage reserve.");
                }
            }
            output.write(buffer, 0, count);
            copiedBytes += count;
            if (bytesCopied != null && count > 0) {
                bytesCopied.accept(count);
            }
        }
    }
}
