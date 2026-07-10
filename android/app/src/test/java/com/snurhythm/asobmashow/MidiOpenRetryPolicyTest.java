package com.snurhythm.asobmashow;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class MidiOpenRetryPolicyTest {
    @Test
    public void capsAutomaticAttemptsPerDevice() {
        MidiOpenRetryPolicy policy = new MidiOpenRetryPolicy();

        assertTrue(policy.beginAttempt(10));
        assertTrue(policy.beginAttempt(10));
        assertTrue(policy.beginAttempt(10));
        assertFalse(policy.beginAttempt(10));
        assertTrue(policy.beginAttempt(11));
    }

    @Test
    public void newTriggerAndRemovalResetOnlyTheirDevice() {
        MidiOpenRetryPolicy policy = new MidiOpenRetryPolicy();
        exhaust(policy, 20);
        exhaust(policy, 21);

        policy.reset(20);
        assertTrue(policy.beginAttempt(20));
        assertFalse(policy.beginAttempt(21));

        policy.remove(21);
        assertTrue(policy.beginAttempt(21));
    }

    @Test
    public void generationShutdownClearsEveryRetryBudget() {
        MidiOpenRetryPolicy policy = new MidiOpenRetryPolicy();
        exhaust(policy, 30);
        exhaust(policy, 31);

        policy.clear();

        assertTrue(policy.beginAttempt(30));
        assertTrue(policy.beginAttempt(31));
    }

    @Test
    public void removedDeviceCannotConsumeAlreadyPostedRetry() {
        MidiOpenRetryPolicy policy = new MidiOpenRetryPolicy();
        assertTrue(policy.beginAttempt(40));
        long removedRetry = policy.scheduleRetry(40);
        assertTrue(removedRetry != 0);

        policy.remove(40);

        assertTrue(policy.beginAttempt(40));
        long replacementRetry = policy.scheduleRetry(40);
        assertTrue(replacementRetry != 0);
        assertTrue(replacementRetry != removedRetry);
        assertFalse(policy.beginScheduledAttempt(40, removedRetry));
        assertTrue(policy.beginScheduledAttempt(40, replacementRetry));
    }

    @Test
    public void scheduledAttemptsShareThePerTriggerCap() {
        MidiOpenRetryPolicy policy = new MidiOpenRetryPolicy();
        assertTrue(policy.beginAttempt(50));

        long secondAttempt = policy.scheduleRetry(50);
        assertTrue(secondAttempt != 0);
        assertTrue(policy.beginScheduledAttempt(50, secondAttempt));
        long thirdAttempt = policy.scheduleRetry(50);
        assertTrue(thirdAttempt != 0);
        assertTrue(policy.beginScheduledAttempt(50, thirdAttempt));
        long cappedAttempt = policy.scheduleRetry(50);
        assertTrue(cappedAttempt != 0);
        assertFalse(policy.beginScheduledAttempt(50, cappedAttempt));
        assertFalse(policy.beginAttempt(50));
    }

    private static void exhaust(MidiOpenRetryPolicy policy, int deviceId) {
        assertTrue(policy.beginAttempt(deviceId));
        assertTrue(policy.beginAttempt(deviceId));
        assertTrue(policy.beginAttempt(deviceId));
        assertFalse(policy.beginAttempt(deviceId));
    }
}
