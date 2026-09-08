package com.snurhythm.asobmashow;

import java.io.IOException;
import java.io.InputStream;
import java.io.InterruptedIOException;
import java.io.OutputStream;
import java.util.function.BooleanSupplier;
import java.util.function.IntSupplier;

final class ChartImportCopyControl {
    private final IntSupplier copyState;
    private final BooleanSupplier cancelled;

    ChartImportCopyControl(IntSupplier copyState, BooleanSupplier cancelled) {
        this.copyState = copyState;
        this.cancelled = cancelled;
    }

    void checkpoint() throws InterruptedIOException {
        while (true) {
            if (cancelled.getAsBoolean() || Thread.currentThread().isInterrupted()) {
                throw new InterruptedIOException("Chart import cancelled.");
            }
            int state = copyState.getAsInt();
            if (state < 0) {
                throw new InterruptedIOException("Chart import cancelled.");
            }
            if (state > 0) {
                return;
            }
            try {
                Thread.sleep(20);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                throw new InterruptedIOException("Chart import cancelled.");
            }
        }
    }

    void copy(InputStream input, OutputStream output) throws IOException {
        byte[] buffer = new byte[1024 * 1024];
        while (true) {
            checkpoint();
            int count = input.read(buffer);
            if (count < 0) {
                return;
            }
            checkpoint();
            output.write(buffer, 0, count);
        }
    }
}
