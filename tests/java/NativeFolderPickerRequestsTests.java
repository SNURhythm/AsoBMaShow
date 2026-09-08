package com.snurhythm.asobmashow;

import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class NativeFolderPickerRequestsTests {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void finish(Thread worker) throws Exception {
        worker.join(2000);
        if (worker.isAlive()) {
            worker.interrupt();
            worker.join(2000);
            throw new AssertionError("Owner cancellation did not release the waiter");
        }
    }

    private static void testResultsArePerRequest() {
        NativeFolderPickerRequests requests = new NativeFolderPickerRequests();
        NativeFolderPickerRequests.Request first = requests.begin(false, () -> false);
        require(first != null, "First request accepted");
        require(requests.begin(false, () -> false) == null, "Concurrent picker rejected");
        requests.complete(first.code, "content://folder\nname\n/storage/name");
        require(requests.await(first).startsWith("content://folder"), "Selection delivered");
        NativeFolderPickerRequests.Request second = requests.begin(false, () -> false);
        require(second != null && first.code != second.code, "Repeated request has a new code");
        requests.complete(first.code, "stale selection");
        require(requests.pending(second.code), "Late result must not release the next request");
        requests.complete(second.code, "__CANCELLED__");
        require(requests.await(second).equals("__CANCELLED__"), "User cancellation delivered");
        NativeFolderPickerRequests.Request permission = requests.begin(true, () -> false);
        require(requests.permission(permission.code), "Permission phase identified");
        requests.complete(permission.code, "1");
        require(requests.await(permission).equals("1"), "Permission success delivered");
        require(!requests.handles(0x41534f44) && !requests.handles(0x41534f46),
                "Archive and folder import pickers retain their result channel");
        NativeFolderPickerRequests nextActivity = new NativeFolderPickerRequests();
        NativeFolderPickerRequests.Request next = nextActivity.begin(false, () -> false);
        require(next.code != first.code && next.code != second.code,
                "Recreated Activity cannot reuse an old request code");
        nextActivity.destroy();
        require(nextActivity.await(next).equals("__CANCELLED__"), "Recreation cancels old wait");
    }

    private static void testOwnerCancellation(boolean permission, boolean dispatchFirst)
            throws Exception {
        NativeFolderPickerRequests requests = new NativeFolderPickerRequests();
        AtomicBoolean cancelled = new AtomicBoolean();
        CountDownLatch awaiting = new CountDownLatch(1);
        NativeFolderPickerRequests.Request request = requests.begin(permission, () -> {
            if (Thread.currentThread().getName().equals("picker-owner")) awaiting.countDown();
            return cancelled.get();
        });
        AtomicBoolean launched = new AtomicBoolean();
        if (dispatchFirst) requests.dispatch(request, () -> launched.set(true));
        AtomicReference<String> result = new AtomicReference<>();
        Thread worker = new Thread(() -> result.set(requests.await(request)), "picker-owner");
        worker.start();
        require(awaiting.await(2, TimeUnit.SECONDS), "Owner entered the wait");
        cancelled.set(true);
        finish(worker);
        requests.dispatch(request, () -> launched.set(true));
        require(launched.get() == dispatchFirst, "Cancelled queued dispatch must not launch");
        require("__CANCELLED__".equals(result.get()), "Owner cancellation returned");
        require(requests.begin(permission, cancelled::get) == null,
                "Cancellation racing registration rejects the request");
        NativeFolderPickerRequests.Request next = requests.begin(false, () -> false);
        require(next != null, "Owner cancellation does not destroy the Activity");
        requests.complete(request.code, "late");
        require(requests.pending(next.code), "Cancelled result cannot complete new picker");
        requests.destroy();
        require(requests.await(next).equals("__CANCELLED__"), "Destroy releases next request");
        require(requests.begin(false, () -> false) == null, "Destroyed Activity stays closed");
    }

    private static void testInterruptedWaitCanBeRetried() throws Exception {
        NativeFolderPickerRequests requests = new NativeFolderPickerRequests();
        NativeFolderPickerRequests.Request request = requests.begin(false, () -> false);
        AtomicBoolean interruptPreserved = new AtomicBoolean();
        Thread worker = new Thread(() -> {
            requests.await(request);
            interruptPreserved.set(Thread.currentThread().isInterrupted());
        });
        worker.start();
        worker.interrupt();
        finish(worker);
        require(interruptPreserved.get(), "Interrupt status preserved");
        NativeFolderPickerRequests.Request next = requests.begin(false, () -> false);
        require(next != null, "Interrupted wait does not leave the picker busy");
        requests.destroy();
        requests.await(next);
    }

    public static void main(String[] arguments) throws Exception {
        testResultsArePerRequest();
        for (boolean permission : new boolean[] {false, true}) {
            for (boolean dispatchFirst : new boolean[] {false, true}) {
                testOwnerCancellation(permission, dispatchFirst);
            }
        }
        testInterruptedWaitCanBeRetried();
        System.out.println("NativeFolderPickerRequests lifecycle tests passed");
    }
}
