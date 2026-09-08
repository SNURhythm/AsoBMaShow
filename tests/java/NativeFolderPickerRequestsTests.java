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

    private static void testResumeIsBoundToDepartingPermission() {
        NativeFolderPickerRequests requests = new NativeFolderPickerRequests();
        NativeFolderPickerRequests.Request queued = requests.begin(true, () -> false);
        requests.onPause();
        requests.onResume(() -> true);
        require(requests.pending(queued.code), "Unlaunched permission survives resume");
        requests.dispatch(queued, () -> {});
        requests.onResume(() -> true);
        require(requests.pending(queued.code), "Launch alone is not a return");
        requests.onPause();
        requests.complete(queued.code, "0");
        require(requests.await(queued).equals("0"), "Activity result wins before resume");
        NativeFolderPickerRequests.Request later = requests.begin(true, () -> false);
        requests.dispatch(later, () -> {});
        requests.onResume(() -> true);
        require(requests.pending(later.code), "Old resume cannot wake later permission");
        requests.onPause();
        requests.onResume(() -> false);
        require(requests.await(later).equals("0"), "Denied permission completes on return");
        requests.complete(queued.code, "1");
        NativeFolderPickerRequests.Request folder = requests.begin(false, () -> false);
        requests.dispatch(folder, () -> {});
        requests.onPause();
        requests.onResume(() -> true);
        require(requests.pending(folder.code), "Folder selection is not permission completion");
        requests.destroy();
        requests.await(folder);
    }

    private static void testResumeRejectsCancelledAndDestroyedPermission() {
        for (boolean destroy : new boolean[] {false, true}) {
            NativeFolderPickerRequests requests = new NativeFolderPickerRequests();
            AtomicBoolean cancelled = new AtomicBoolean();
            NativeFolderPickerRequests.Request request = requests.begin(true, cancelled::get);
            requests.dispatch(request, () -> {});
            requests.onPause();
            if (destroy) requests.destroy();
            else cancelled.set(true);
            requests.onResume(() -> {
                throw new AssertionError("Inactive permission must not query access");
            });
            require(requests.await(request).equals("__CANCELLED__"),
                    "Resume cannot overwrite cancellation or destruction");
            if (!destroy) {
                NativeFolderPickerRequests.Request next = requests.begin(true, () -> false);
                requests.dispatch(next, () -> {});
                requests.onResume(() -> true);
                require(requests.pending(next.code), "Cancelled return cannot wake next owner");
                requests.onPause();
                requests.onResume(() -> true);
                require(requests.await(next).equals("1"), "Next owner's own return grants access");
            }
        }
    }

    public static void main(String[] arguments) throws Exception {
        testResumeIsBoundToDepartingPermission();
        testResumeRejectsCancelledAndDestroyedPermission();
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
