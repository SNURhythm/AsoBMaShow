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

    private static void exhaust(MidiOpenRetryPolicy policy, int deviceId) {
        assertTrue(policy.beginAttempt(deviceId));
        assertTrue(policy.beginAttempt(deviceId));
        assertTrue(policy.beginAttempt(deviceId));
        assertFalse(policy.beginAttempt(deviceId));
    }
}
