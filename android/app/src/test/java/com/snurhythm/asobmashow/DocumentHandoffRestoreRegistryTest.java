package com.snurhythm.asobmashow;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertEquals;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.Test;

public class DocumentHandoffRestoreRegistryTest {
    @Test
    public void delayedOldRestoreFinishesBeforeNewSameUriWrite() throws Exception {
        DocumentHandoffRestoreRegistry registry =
                new DocumentHandoffRestoreRegistry();
        DocumentHandoffRestoreRegistry.Ticket old =
                registry.acquire("content://provider/profile");
        AtomicReference<String> providerContents =
                new AtomicReference<>("partial-old-write");
        CountDownLatch newWriteStarted = new CountDownLatch(1);
        Thread next = new Thread(() -> {
            try {
                DocumentHandoffRestoreRegistry.Ticket current =
                        registry.acquire("content://provider/profile");
                newWriteStarted.countDown();
                providerContents.set("new-success");
                registry.complete(current);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        });
        next.start();
        Thread.sleep(25);
        assertEquals(1, newWriteStarted.getCount());

        providerContents.set("");
        registry.complete(old);
        next.join(1000);
        assertEquals(0, newWriteStarted.getCount());
        assertFalse(next.isAlive());
        assertEquals("new-success", providerContents.get());
    }

    @Test
    public void interruptedWaiterNeverAcquiresOrReleasesOldRestore() throws Exception {
        DocumentHandoffRestoreRegistry registry =
                new DocumentHandoffRestoreRegistry();
        DocumentHandoffRestoreRegistry.Ticket old =
                registry.acquire("content://provider/profile");
        CountDownLatch interrupted = new CountDownLatch(1);
        Thread waiter = new Thread(() -> {
            try {
                registry.acquire("content://provider/profile");
            } catch (InterruptedException expected) {
                interrupted.countDown();
            }
        });
        waiter.start();
        Thread.sleep(25);
        waiter.interrupt();
        waiter.join(1000);
        assertEquals(0, interrupted.getCount());

        CountDownLatch laterAcquired = new CountDownLatch(1);
        Thread later = new Thread(() -> {
            try {
                DocumentHandoffRestoreRegistry.Ticket ticket =
                        registry.acquire("content://provider/profile");
                laterAcquired.countDown();
                registry.complete(ticket);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        });
        later.start();
        Thread.sleep(25);
        assertEquals(1, laterAcquired.getCount());
        registry.complete(old);
        later.join(1000);
        assertEquals(0, laterAcquired.getCount());
    }

    @Test
    public void cancelledWaiterReturnsWithoutReleasingOldRestore()
            throws Exception {
        DocumentHandoffRestoreRegistry registry =
                new DocumentHandoffRestoreRegistry();
        DocumentHandoffRestoreRegistry.Ticket old =
                registry.acquire("content://provider/profile");
        AtomicBoolean cancelled = new AtomicBoolean(false);
        AtomicReference<DocumentHandoffRestoreRegistry.Ticket> acquired =
                new AtomicReference<>();
        Thread waiter = new Thread(() -> {
            try {
                acquired.set(registry.acquire(
                        "content://provider/profile", cancelled::get));
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        });
        waiter.start();
        Thread.sleep(25);
        cancelled.set(true);
        waiter.join(1000);
        assertFalse(waiter.isAlive());
        assertEquals(null, acquired.get());

        CountDownLatch laterAcquired = new CountDownLatch(1);
        Thread later = new Thread(() -> {
            try {
                DocumentHandoffRestoreRegistry.Ticket ticket =
                        registry.acquire("content://provider/profile");
                laterAcquired.countDown();
                registry.complete(ticket);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        });
        later.start();
        Thread.sleep(25);
        assertEquals(1, laterAcquired.getCount());
        registry.complete(old);
        later.join(1000);
        assertEquals(0, laterAcquired.getCount());
    }
}
