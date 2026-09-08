package com.snurhythm.asobmashow;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

final class NativeFolderPickerRequests {
    private static final String CANCELLED = "__CANCELLED__";
    private static final DocumentHandoffRequestCodeAllocator CODES =
            new DocumentHandoffRequestCodeAllocator(0x4300, 0x52ff);

    static final class Request {
        final int code;
        final boolean permission;
        final BooleanSupplier cancelled;
        final CountDownLatch done = new CountDownLatch(1);
        String result = CANCELLED;

        Request(int code, boolean permission, BooleanSupplier cancelled) {
            this.code = code;
            this.permission = permission;
            this.cancelled = cancelled;
        }
    }

    private Request active;
    private boolean destroyed;

    synchronized boolean cancelled(BooleanSupplier ownerCancelled) {
        return destroyed || ownerCancelled.getAsBoolean();
    }

    synchronized Request begin(boolean permission, BooleanSupplier ownerCancelled) {
        if (cancelled(ownerCancelled) || active != null) return null;
        int code = CODES.allocate();
        if (code < 0) return null;
        active = new Request(code, permission, ownerCancelled);
        return active;
    }

    synchronized void dispatch(Request request, Runnable launch) {
        if (active != request || request.done.getCount() == 0) return;
        if (cancelled(request.cancelled)) {
            request.done.countDown();
            return;
        }
        launch.run();
    }

    String await(Request request) {
        if (request == null) return CANCELLED;
        try {
            while (true) {
                synchronized (this) {
                    if (cancelled(request.cancelled)) return CANCELLED;
                    if (request.done.getCount() == 0) return request.result;
                }
                request.done.await(25, TimeUnit.MILLISECONDS);
            }
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            return CANCELLED;
        } finally {
            synchronized (this) {
                request.done.countDown();
                if (active == request) active = null;
            }
        }
    }

    boolean handles(int code) {
        return CODES.wasIssued(code);
    }

    synchronized boolean pending(int code) {
        return active != null && active.code == code && active.done.getCount() != 0
                && !cancelled(active.cancelled);
    }

    synchronized boolean permission(int code) {
        return pending(code) && active.permission;
    }

    synchronized void complete(int code, String result) {
        if (!pending(code)) return;
        active.result = result;
        active.done.countDown();
    }

    synchronized void destroy() {
        destroyed = true;
        if (active != null) {
            active.result = CANCELLED;
            active.done.countDown();
        }
    }
}
