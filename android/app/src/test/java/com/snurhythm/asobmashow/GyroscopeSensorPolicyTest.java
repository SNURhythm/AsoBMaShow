package com.snurhythm.asobmashow;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public final class GyroscopeSensorPolicyTest {
    private static final double EPSILON = 0.000001;

    @Test
    public void registrationRequiresNativeStartAndResumedActivity() {
        GyroscopeSensorPolicy policy = new GyroscopeSensorPolicy();

        assertTransition(policy.setNativeStarted(true),
                GyroscopeSensorPolicy.RegistrationAction.NONE, 0);
        assertTransition(policy.setNativeStarted(true),
                GyroscopeSensorPolicy.RegistrationAction.NONE, 0);
        assertTransition(policy.setActivityResumed(true),
                GyroscopeSensorPolicy.RegistrationAction.REGISTER, 1);
        assertTrue(policy.shouldRegister());
        assertTrue(policy.acceptsCallback(1));

        assertTransition(policy.setActivityResumed(true),
                GyroscopeSensorPolicy.RegistrationAction.NONE, 1);
        assertTransition(policy.setNativeStarted(true),
                GyroscopeSensorPolicy.RegistrationAction.NONE, 1);

        assertTransition(policy.setActivityResumed(false),
                GyroscopeSensorPolicy.RegistrationAction.UNREGISTER, 2);
        assertFalse(policy.shouldRegister());
        assertFalse(policy.acceptsCallback(1));
        assertTransition(policy.setActivityResumed(false),
                GyroscopeSensorPolicy.RegistrationAction.NONE, 2);

        assertTransition(policy.setActivityResumed(true),
                GyroscopeSensorPolicy.RegistrationAction.REGISTER, 3);
        assertTrue(policy.acceptsCallback(3));
        assertTransition(policy.setNativeStarted(false),
                GyroscopeSensorPolicy.RegistrationAction.UNREGISTER, 4);
        assertFalse(policy.acceptsCallback(3));
    }

    @Test
    public void failedRegistrationInvalidatesGenerationAndCanRetry() {
        GyroscopeSensorPolicy policy = registeredPolicy();
        assertTrue(policy.acceptsCallback(1));

        assertTrue(policy.registrationFailed(1));
        assertFalse(policy.acceptsCallback(1));
        assertEquals(2, policy.currentGeneration());
        assertFalse(policy.registrationFailed(1));

        assertTransition(policy.setNativeStarted(true),
                GyroscopeSensorPolicy.RegistrationAction.REGISTER, 3);
        assertTrue(policy.acceptsCallback(3));
    }

    @Test
    public void unreliableAndLowAccuracyAreNeverUsable() {
        GyroscopeSensorPolicy policy = registeredPolicy();

        GyroscopeSensorPolicy.RotationDecision unreliable = policy.evaluateRotationVector(
                1, GyroscopeSensorPolicy.ACCURACY_UNRELIABLE, 0.0,
                false, 0.0);
        assertTrue(unreliable.callbackAccepted);
        assertFalse(unreliable.usableAccuracy);

        GyroscopeSensorPolicy.RotationDecision low = policy.evaluateRotationVector(
                1, GyroscopeSensorPolicy.ACCURACY_LOW, 0.0,
                false, 0.0);
        assertTrue(low.callbackAccepted);
        assertFalse(low.usableAccuracy);
        assertTrue(low.accuracyGeneration > unreliable.accuracyGeneration);
    }

    @Test
    public void mediumAndHighNeedNoOptionalHeadingEstimate() {
        GyroscopeSensorPolicy policy = registeredPolicy();

        GyroscopeSensorPolicy.RotationDecision medium = policy.evaluateRotationVector(
                1, GyroscopeSensorPolicy.ACCURACY_MEDIUM, Math.PI / 2.0,
                false, 0.0);
        assertTrue(medium.usableAccuracy);
        assertEquals(90.0, medium.headingDegrees, EPSILON);

        GyroscopeSensorPolicy.RotationDecision high = policy.evaluateRotationVector(
                1, GyroscopeSensorPolicy.ACCURACY_HIGH, -Math.PI / 2.0,
                false, 0.0);
        assertTrue(high.usableAccuracy);
        assertEquals(270.0, high.headingDegrees, EPSILON);
        assertTrue(high.accuracyGeneration > medium.accuracyGeneration);
    }

    @Test
    public void headingConfidenceIncludesFifteenDegreesAndConvertsRadians() {
        GyroscopeSensorPolicy policy = registeredPolicy();
        double fifteenDegrees = Math.toRadians(15.0);

        GyroscopeSensorPolicy.RotationDecision boundary = policy.evaluateRotationVector(
                1, GyroscopeSensorPolicy.ACCURACY_MEDIUM, 0.0,
                true, fifteenDegrees);
        assertTrue(boundary.usableAccuracy);
        assertEquals(15.0, boundary.headingAccuracyDegrees, EPSILON);

        GyroscopeSensorPolicy.RotationDecision outside = policy.evaluateRotationVector(
                1, GyroscopeSensorPolicy.ACCURACY_MEDIUM, 0.0,
                true, Math.toRadians(15.01));
        assertFalse(outside.usableAccuracy);
        assertTrue(outside.accuracyGeneration > boundary.accuracyGeneration);

        GyroscopeSensorPolicy.RotationDecision nonFinite = policy.evaluateRotationVector(
                1, GyroscopeSensorPolicy.ACCURACY_HIGH, 0.0,
                true, Double.NaN);
        assertFalse(nonFinite.usableAccuracy);
    }

    @Test
    public void fiveDegreeEstimateChangeIsMaterial() {
        GyroscopeSensorPolicy policy = registeredPolicy();

        GyroscopeSensorPolicy.RotationDecision initial = policy.evaluateRotationVector(
                1, GyroscopeSensorPolicy.ACCURACY_MEDIUM, 0.0,
                true, Math.toRadians(4.0));
        GyroscopeSensorPolicy.RotationDecision gradualChange =
                policy.evaluateRotationVector(
                        1, GyroscopeSensorPolicy.ACCURACY_MEDIUM, 0.0,
                        true, Math.toRadians(6.0));
        GyroscopeSensorPolicy.RotationDecision smallChange = policy.evaluateRotationVector(
                1, GyroscopeSensorPolicy.ACCURACY_MEDIUM, 0.0,
                true, Math.toRadians(8.99));
        assertEquals(initial.accuracyGeneration, gradualChange.accuracyGeneration);
        assertEquals(initial.accuracyGeneration, smallChange.accuracyGeneration);

        GyroscopeSensorPolicy.RotationDecision materialChange =
                policy.evaluateRotationVector(
                        1, GyroscopeSensorPolicy.ACCURACY_MEDIUM, 0.0,
                        true, Math.toRadians(9.0));
        assertTrue(materialChange.accuracyGeneration > smallChange.accuracyGeneration);
    }

    @Test
    public void azimuthNormalizationCoversPositiveNegativeAndWrap() {
        assertEquals(0.0, GyroscopeSensorPolicy.headingDegrees(0.0), EPSILON);
        assertEquals(90.0, GyroscopeSensorPolicy.headingDegrees(Math.PI / 2.0),
                EPSILON);
        assertEquals(270.0, GyroscopeSensorPolicy.headingDegrees(-Math.PI / 2.0),
                EPSILON);
        assertEquals(5.0, GyroscopeSensorPolicy.headingDegrees(
                2.0 * Math.PI + Math.toRadians(5.0)), EPSILON);
        assertEquals(355.0, GyroscopeSensorPolicy.headingDegrees(
                -2.0 * Math.PI - Math.toRadians(5.0)), EPSILON);
    }

    @Test
    public void staleCallbackCannotChangeAccuracyState() {
        GyroscopeSensorPolicy policy = registeredPolicy();
        GyroscopeSensorPolicy.RotationDecision current = policy.evaluateRotationVector(
                1, GyroscopeSensorPolicy.ACCURACY_MEDIUM, 0.0,
                true, Math.toRadians(3.0));

        assertTransition(policy.setActivityResumed(false),
                GyroscopeSensorPolicy.RegistrationAction.UNREGISTER, 2);
        GyroscopeSensorPolicy.RotationDecision stale = policy.evaluateRotationVector(
                1, GyroscopeSensorPolicy.ACCURACY_HIGH, Math.PI,
                true, Math.toRadians(12.0));
        assertFalse(stale.callbackAccepted);
        assertTrue(stale.accuracyGeneration > current.accuracyGeneration);

        GyroscopeSensorPolicy.RotationDecision repeatedStale =
                policy.evaluateRotationVector(
                        1, GyroscopeSensorPolicy.ACCURACY_HIGH, -Math.PI,
                        true, Math.toRadians(2.0));
        assertFalse(repeatedStale.callbackAccepted);
        assertEquals(stale.accuracyGeneration,
                repeatedStale.accuracyGeneration);
    }

    @Test
    public void gyroscopePairMustBeAtOrBeforeAndNoMoreThanFiftyMillisecondsOld() {
        long rotationTimestamp = 2_000_000_000L;

        assertTrue(GyroscopeSensorPolicy.canPairGyroscope(
                rotationTimestamp - 50_000_000L, rotationTimestamp));
        assertTrue(GyroscopeSensorPolicy.canPairGyroscope(
                rotationTimestamp, rotationTimestamp));
        assertFalse(GyroscopeSensorPolicy.canPairGyroscope(
                rotationTimestamp - 50_000_001L, rotationTimestamp));
        assertFalse(GyroscopeSensorPolicy.canPairGyroscope(
                rotationTimestamp + 1L, rotationTimestamp));
    }

    @Test
    public void sensorTimestampAndWorldVerticalRateUseCanonicalUnitsAndSign() {
        assertEquals(1.5,
                GyroscopeSensorPolicy.sensorTimestampSeconds(1_500_000_000L),
                EPSILON);

        float[] identity = {
                1.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 1.0f
        };
        assertEquals(-Math.toDegrees(1.0),
                GyroscopeSensorPolicy.clockwiseWorldZRateDegreesPerSecond(
                        identity, 0.0f, 0.0f, 1.0f),
                EPSILON);

        float[] deviceXProjectsToWorldZ = {
                0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 1.0f,
                1.0f, 0.0f, 0.0f
        };
        assertEquals(-Math.toDegrees(2.0),
                GyroscopeSensorPolicy.clockwiseWorldZRateDegreesPerSecond(
                        deviceXProjectsToWorldZ, 2.0f, 0.0f, 0.0f),
                EPSILON);
    }

    @Test
    public void unpairedRotationCanReleaseCalibrationButCannotAdvanceMotion() {
        assertEquals(GyroscopeSensorPolicy.RotationDelivery.DROP,
                GyroscopeSensorPolicy.rotationDelivery(true, false, true));
        assertEquals(GyroscopeSensorPolicy.RotationDelivery.CALIBRATION_ONLY,
                GyroscopeSensorPolicy.rotationDelivery(false, false, true));
        assertEquals(GyroscopeSensorPolicy.RotationDelivery.DROP,
                GyroscopeSensorPolicy.rotationDelivery(false, false, false));
        assertEquals(GyroscopeSensorPolicy.RotationDelivery.PAIRED,
                GyroscopeSensorPolicy.rotationDelivery(true, true, false));
        assertEquals(GyroscopeSensorPolicy.RotationDelivery.PAIRED,
                GyroscopeSensorPolicy.rotationDelivery(false, true, false));
    }

    private static GyroscopeSensorPolicy registeredPolicy() {
        GyroscopeSensorPolicy policy = new GyroscopeSensorPolicy();
        policy.setNativeStarted(true);
        assertTransition(policy.setActivityResumed(true),
                GyroscopeSensorPolicy.RegistrationAction.REGISTER, 1);
        return policy;
    }

    private static void assertTransition(
            GyroscopeSensorPolicy.RegistrationDecision decision,
            GyroscopeSensorPolicy.RegistrationAction action,
            long generation) {
        assertEquals(action, decision.action);
        assertEquals(generation, decision.generation);
    }
}
