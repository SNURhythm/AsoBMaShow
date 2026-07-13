package com.snurhythm.asobmashow;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertTrue;

import java.io.IOException;
import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;
import org.junit.Test;

public class DocumentHandoffRollbackCoordinatorTest {
    private static final long TIMEOUT_SECONDS = 5;
    private static final String URI = "content://provider/profile";

    @Test
    public void finishReleasesLeaseAndDuplicateResolutionCannotReleaseNewerLease()
            throws Exception {
        List<Throwable> failures = new CopyOnWriteArrayList<>();
        DocumentHandoffRollbackCoordinator coordinator =
                new DocumentHandoffRollbackCoordinator(
                        new DocumentHandoffRestoreRegistry(),
                        Runnable::run,
                        failures::add);
        AtomicInteger oldRestoreCount = new AtomicInteger();
        CountDownLatch cancellationObserved = new CountDownLatch(1);
        CountDownLatch releaseCancellation = new CountDownLatch(1);

        DocumentHandoffRollbackCoordinator.Lease first =
                coordinator.acquire(URI, () -> false);
        coordinator.finish(first);
        coordinator.finish(first);

        DocumentHandoffRollbackCoordinator.Lease second =
                coordinator.acquire(URI, () -> false);
        coordinator.rollback(first, () -> {
            oldRestoreCount.incrementAndGet();
            return true;
        });
        assertEquals(0, oldRestoreCount.get());

        DaemonAcquire waiter = acquireOnDaemon(
                coordinator,
                URI,
                blockingCancellationSupplier(
                        cancellationObserved, releaseCancellation));
        try {
            assertTrue(
                    "waiter never observed the pending second lease",
                    cancellationObserved.await(
                            TIMEOUT_SECONDS, TimeUnit.SECONDS));
            assertNull(waiter.lease.get());
            assertEquals(1, waiter.completed.getCount());

            releaseCancellation.countDown();
            coordinator.finish(second);

            assertNotNull(waiter.awaitLease());
            assertTrue(failures.isEmpty());
        } finally {
            releaseCancellation.countDown();
            coordinator.finish(second);
            waiter.awaitCleanup();
        }
    }

    @Test
    public void sameUriWaiterAcquiresOnlyAfterRollbackTaskFinishes()
            throws Exception {
        AtomicReference<Runnable> rollbackTask = new AtomicReference<>();
        List<Throwable> failures = new CopyOnWriteArrayList<>();
        DocumentHandoffRollbackCoordinator coordinator =
                new DocumentHandoffRollbackCoordinator(
                        new DocumentHandoffRestoreRegistry(),
                        rollbackTask::set,
                        failures::add);
        CountDownLatch rollbackEntered = new CountDownLatch(1);
        CountDownLatch releaseRollback = new CountDownLatch(1);
        CountDownLatch rollbackTaskFinished = new CountDownLatch(1);
        CountDownLatch releaseFirstWaiterCheck = new CountDownLatch(1);
        BlockingQueue<String> waiterEvents = new LinkedBlockingQueue<>();
        AtomicInteger cancellationChecks = new AtomicInteger();

        DocumentHandoffRollbackCoordinator.Lease first =
                coordinator.acquire(URI, () -> false);
        coordinator.rollback(first, () -> {
            rollbackEntered.countDown();
            if (!releaseRollback.await(TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
                throw new IOException("timed out waiting to release rollback");
            }
            return true;
        });
        assertNotNull(rollbackTask.get());

        Thread rollbackRunner = new Thread(() -> {
            try {
                rollbackTask.get().run();
            } finally {
                rollbackTaskFinished.countDown();
            }
        }, "document-rollback-test-runner");
        rollbackRunner.setDaemon(true);
        rollbackRunner.start();

        assertTrue(
                "rollback action never entered",
                rollbackEntered.await(TIMEOUT_SECONDS, TimeUnit.SECONDS));
        DaemonAcquire waiter = acquireOnDaemon(
                coordinator,
                URI,
                () -> {
                    int check = cancellationChecks.incrementAndGet();
                    waiterEvents.add("check-" + check);
                    if (check == 1) {
                        try {
                            if (!releaseFirstWaiterCheck.await(
                                    TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
                                throw new AssertionError(
                                        "timed out releasing first waiter check");
                            }
                        } catch (InterruptedException failure) {
                            Thread.currentThread().interrupt();
                            throw new AssertionError(failure);
                        }
                    }
                    return false;
                },
                waiterEvents);
        try {
            assertEquals(
                    "waiter never checked the pending same-URI lease",
                    "check-1",
                    waiterEvents.poll(TIMEOUT_SECONDS, TimeUnit.SECONDS));
            assertNull(waiter.lease.get());
            assertEquals(1, waiter.completed.getCount());
            assertEquals(1, rollbackTaskFinished.getCount());

            releaseFirstWaiterCheck.countDown();
            assertEquals(
                    "waiter acquired before the blocked rollback action finished",
                    "check-2",
                    waiterEvents.poll(TIMEOUT_SECONDS, TimeUnit.SECONDS));
            assertNull(waiter.lease.get());
            assertEquals(1, waiter.completed.getCount());
            assertEquals(1, rollbackTaskFinished.getCount());

            releaseRollback.countDown();
            assertTrue(
                    "rollback task did not finish",
                    rollbackTaskFinished.await(
                            TIMEOUT_SECONDS, TimeUnit.SECONDS));
            assertNotNull(waiter.awaitLease());
            assertTrue(failures.isEmpty());
        } finally {
            releaseFirstWaiterCheck.countDown();
            releaseRollback.countDown();
            coordinator.finish(first);
            waiter.awaitCleanup();
            rollbackRunner.join(
                    TimeUnit.SECONDS.toMillis(TIMEOUT_SECONDS));
            assertFalse(
                    "rollback runner remained alive",
                    rollbackRunner.isAlive());
        }
    }

    @Test
    public void falseAndThrownRestoreFailuresAreReportedAndReleaseLeases()
            throws Exception {
        List<Throwable> failures = new CopyOnWriteArrayList<>();
        DocumentHandoffRollbackCoordinator coordinator =
                new DocumentHandoffRollbackCoordinator(
                        new DocumentHandoffRestoreRegistry(),
                        Runnable::run,
                        failures::add);
        String falseUri = "content://provider/false";
        String thrownUri = "content://provider/thrown";
        IOException knownFailure = new IOException("known restore failure");

        DocumentHandoffRollbackCoordinator.Lease falseLease =
                coordinator.acquire(falseUri, () -> false);
        coordinator.rollback(falseLease, () -> false);

        DocumentHandoffRollbackCoordinator.Lease thrownLease =
                coordinator.acquire(thrownUri, () -> false);
        coordinator.rollback(thrownLease, () -> {
            throw knownFailure;
        });

        assertEquals(2, failures.size());
        assertTrue(failures.get(0) instanceof IOException);
        assertEquals(
                "The empty export destination could not be restored.",
                failures.get(0).getMessage());
        assertSame(knownFailure, failures.get(1));

        DocumentHandoffRollbackCoordinator.Lease reacquiredFalse =
                coordinator.acquire(falseUri, () -> false);
        DocumentHandoffRollbackCoordinator.Lease reacquiredThrown =
                coordinator.acquire(thrownUri, () -> false);
        assertNotNull(reacquiredFalse);
        assertNotNull(reacquiredThrown);
        coordinator.finish(reacquiredFalse);
        coordinator.finish(reacquiredThrown);
    }

    @Test
    public void defaultExecutorRunsOverflowRollbackOnCallingThread()
            throws Exception {
        List<Throwable> failures = new CopyOnWriteArrayList<>();
        DocumentHandoffRollbackCoordinator coordinator =
                DocumentHandoffRollbackCoordinator.createDefault(failures::add);
        CountDownLatch workerStarted = new CountDownLatch(1);
        CountDownLatch releaseWorker = new CountDownLatch(1);
        CountDownLatch queuedFinished = new CountDownLatch(8);
        AtomicReference<Thread> overflowThread = new AtomicReference<>();
        Thread callingThread = Thread.currentThread();

        DocumentHandoffRollbackCoordinator.Lease first =
                coordinator.acquire("content://provider/worker", () -> false);
        coordinator.rollback(first, () -> {
            workerStarted.countDown();
            if (!releaseWorker.await(TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
                throw new IOException("timed out waiting to release worker");
            }
            return true;
        });

        try {
            assertTrue(
                    "default rollback worker did not start",
                    workerStarted.await(TIMEOUT_SECONDS, TimeUnit.SECONDS));

            for (int index = 0; index < 8; index++) {
                DocumentHandoffRollbackCoordinator.Lease queued =
                        coordinator.acquire(
                                "content://provider/queued/" + index,
                                () -> false);
                coordinator.rollback(queued, () -> {
                    queuedFinished.countDown();
                    return true;
                });
            }

            DocumentHandoffRollbackCoordinator.Lease overflow =
                    coordinator.acquire(
                            "content://provider/overflow", () -> false);
            coordinator.rollback(overflow, () -> {
                overflowThread.set(Thread.currentThread());
                return true;
            });

            assertSame(callingThread, overflowThread.get());
        } finally {
            releaseWorker.countDown();
            assertTrue(
                    "queued rollback actions did not finish",
                    queuedFinished.await(TIMEOUT_SECONDS, TimeUnit.SECONDS));
        }
        assertTrue(failures.isEmpty());
    }

    @Test
    public void executorThrowAfterRetainingTaskRunsRestoreOnlyOnce()
            throws Exception {
        AtomicReference<Runnable> retainedTask = new AtomicReference<>();
        AtomicInteger restoreCount = new AtomicInteger();
        List<Throwable> failures = new CopyOnWriteArrayList<>();
        RejectedExecutionException rejection =
                new RejectedExecutionException("retained then rejected");
        DocumentHandoffRollbackCoordinator coordinator =
                new DocumentHandoffRollbackCoordinator(
                        new DocumentHandoffRestoreRegistry(),
                        command -> {
                            retainedTask.set(command);
                            throw rejection;
                        },
                        failures::add);

        DocumentHandoffRollbackCoordinator.Lease lease =
                coordinator.acquire(URI, () -> false);
        coordinator.rollback(lease, () -> {
            restoreCount.incrementAndGet();
            return true;
        });

        assertEquals(1, restoreCount.get());
        DocumentHandoffRollbackCoordinator.Lease reacquired =
                coordinator.acquire(URI, () -> false);
        assertNotNull(reacquired);
        coordinator.finish(reacquired);

        assertNotNull(retainedTask.get());
        retainedTask.get().run();
        assertEquals(1, restoreCount.get());
        assertTrue(failures.isEmpty());
    }

    @Test
    public void executorRunThenThrowRunsRestoreOnlyOnce() throws Exception {
        AtomicInteger restoreCount = new AtomicInteger();
        List<Throwable> failures = new CopyOnWriteArrayList<>();
        RejectedExecutionException rejection =
                new RejectedExecutionException("ran then rejected");
        DocumentHandoffRollbackCoordinator coordinator =
                new DocumentHandoffRollbackCoordinator(
                        new DocumentHandoffRestoreRegistry(),
                        command -> {
                            command.run();
                            throw rejection;
                        },
                        failures::add);

        DocumentHandoffRollbackCoordinator.Lease lease =
                coordinator.acquire(URI, () -> false);
        coordinator.rollback(lease, () -> {
            restoreCount.incrementAndGet();
            return true;
        });

        assertEquals(1, restoreCount.get());
        DocumentHandoffRollbackCoordinator.Lease reacquired =
                coordinator.acquire(URI, () -> false);
        assertNotNull(reacquired);
        coordinator.finish(reacquired);
        assertTrue(failures.isEmpty());
    }

    private static BooleanSupplier blockingCancellationSupplier(
            CountDownLatch observed, CountDownLatch release) {
        return () -> {
            observed.countDown();
            try {
                if (!release.await(TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
                    throw new AssertionError(
                            "timed out waiting to release cancellation supplier");
                }
            } catch (InterruptedException failure) {
                Thread.currentThread().interrupt();
                throw new AssertionError(failure);
            }
            return false;
        };
    }

    private static DaemonAcquire acquireOnDaemon(
            DocumentHandoffRollbackCoordinator coordinator,
            String key,
            BooleanSupplier cancelled) {
        return acquireOnDaemon(coordinator, key, cancelled, null);
    }

    private static DaemonAcquire acquireOnDaemon(
            DocumentHandoffRollbackCoordinator coordinator,
            String key,
            BooleanSupplier cancelled,
            BlockingQueue<String> events) {
        DaemonAcquire result = new DaemonAcquire();
        Thread thread = new Thread(() -> {
            try {
                DocumentHandoffRollbackCoordinator.Lease lease =
                        coordinator.acquire(key, cancelled);
                result.lease.set(lease);
                if (events != null) {
                    events.add("acquired");
                }
                coordinator.finish(lease);
            } catch (Throwable failure) {
                result.failure.set(failure);
            } finally {
                result.completed.countDown();
            }
        }, "document-rollback-test-waiter");
        thread.setDaemon(true);
        result.thread = thread;
        thread.start();
        return result;
    }

    private static final class DaemonAcquire {
        final AtomicReference<DocumentHandoffRollbackCoordinator.Lease> lease =
                new AtomicReference<>();
        final AtomicReference<Throwable> failure = new AtomicReference<>();
        final CountDownLatch completed = new CountDownLatch(1);
        Thread thread;

        DocumentHandoffRollbackCoordinator.Lease awaitLease()
                throws Exception {
            assertTrue(
                    "daemon waiter did not complete",
                    completed.await(TIMEOUT_SECONDS, TimeUnit.SECONDS));
            rethrowFailure();
            return lease.get();
        }

        void awaitCleanup() throws Exception {
            assertTrue(
                    "daemon waiter remained blocked during cleanup",
                    completed.await(TIMEOUT_SECONDS, TimeUnit.SECONDS));
            thread.join(TimeUnit.SECONDS.toMillis(TIMEOUT_SECONDS));
            assertFalse("daemon waiter remained alive", thread.isAlive());
            rethrowFailure();
        }

        private void rethrowFailure() throws Exception {
            Throwable thrown = failure.get();
            if (thrown == null) {
                return;
            }
            if (thrown instanceof Exception) {
                throw (Exception) thrown;
            }
            if (thrown instanceof Error) {
                throw (Error) thrown;
            }
            throw new AssertionError(thrown);
        }
    }
}
