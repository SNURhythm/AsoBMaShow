package com.snurhythm.asobmashow;

final class GyroscopeSensorPolicy {
    static final int ACCURACY_UNRELIABLE = 0;
    static final int ACCURACY_LOW = 1;
    static final int ACCURACY_MEDIUM = 2;
    static final int ACCURACY_HIGH = 3;

    private static final double MAX_HEADING_ACCURACY_DEGREES = 15.0;
    private static final double MATERIAL_HEADING_ACCURACY_CHANGE_DEGREES = 5.0;
    private static final long MAX_GYROSCOPE_PAIR_AGE_NANOS = 50_000_000L;

    enum RegistrationAction {
        NONE,
        REGISTER,
        UNREGISTER
    }

    enum RotationDelivery {
        DROP,
        CALIBRATION_ONLY,
        PAIRED
    }

    static final class RegistrationDecision {
        final RegistrationAction action;
        final long generation;

        RegistrationDecision(RegistrationAction action, long generation) {
            this.action = action;
            this.generation = generation;
        }
    }

    static final class RotationDecision {
        final boolean callbackAccepted;
        final boolean usableAccuracy;
        final double headingDegrees;
        final double headingAccuracyDegrees;
        final long accuracyGeneration;

        RotationDecision(boolean callbackAccepted, boolean usableAccuracy,
                         double headingDegrees, double headingAccuracyDegrees,
                         long accuracyGeneration) {
            this.callbackAccepted = callbackAccepted;
            this.usableAccuracy = usableAccuracy;
            this.headingDegrees = headingDegrees;
            this.headingAccuracyDegrees = headingAccuracyDegrees;
            this.accuracyGeneration = accuracyGeneration;
        }
    }

    private enum ConfidenceBoundary {
        UNAVAILABLE,
        WITHIN,
        OUTSIDE
    }

    private boolean nativeStarted;
    private boolean activityResumed;
    private boolean registrationActive;
    private long generation;
    private long accuracyGeneration = 1;
    private int lastAccuracyTier = -1;
    private ConfidenceBoundary lastConfidenceBoundary =
            ConfidenceBoundary.UNAVAILABLE;
    private double headingAccuracyAnchorDegrees = Double.NaN;

    RegistrationDecision setNativeStarted(boolean started) {
        nativeStarted = started;
        return reconcileRegistration();
    }

    RegistrationDecision setActivityResumed(boolean resumed) {
        activityResumed = resumed;
        return reconcileRegistration();
    }

    boolean registrationFailed(long callbackGeneration) {
        if (!registrationActive || callbackGeneration != generation) {
            return false;
        }
        registrationActive = false;
        incrementGeneration();
        resetAccuracyTracking();
        return true;
    }

    boolean shouldRegister() {
        return registrationActive;
    }

    long currentGeneration() {
        return generation;
    }

    boolean acceptsCallback(long callbackGeneration) {
        return registrationActive && callbackGeneration == generation;
    }

    RotationDecision evaluateRotationVector(
            long callbackGeneration, int accuracyTier, double azimuthRadians,
            boolean hasHeadingAccuracy, double headingAccuracyRadians) {
        if (!acceptsCallback(callbackGeneration)) {
            return new RotationDecision(false, false, Double.NaN, Double.NaN,
                    accuracyGeneration);
        }

        double headingAccuracyDegrees = hasHeadingAccuracy
                ? Math.toDegrees(headingAccuracyRadians)
                : Double.NaN;
        ConfidenceBoundary confidenceBoundary = confidenceBoundary(
                hasHeadingAccuracy, headingAccuracyDegrees);

        boolean generationChanged = false;
        if (lastAccuracyTier >= 0 && lastAccuracyTier != accuracyTier) {
            generationChanged = true;
        }
        if (lastConfidenceBoundary != ConfidenceBoundary.UNAVAILABLE
                && confidenceBoundary != ConfidenceBoundary.UNAVAILABLE
                && lastConfidenceBoundary != confidenceBoundary) {
            generationChanged = true;
        }
        if (Double.isFinite(headingAccuracyAnchorDegrees)
                && Double.isFinite(headingAccuracyDegrees)
                && Math.abs(headingAccuracyDegrees - headingAccuracyAnchorDegrees)
                >= MATERIAL_HEADING_ACCURACY_CHANGE_DEGREES) {
            generationChanged = true;
        }
        if (generationChanged) {
            incrementAccuracyGeneration();
        }

        lastAccuracyTier = accuracyTier;
        lastConfidenceBoundary = confidenceBoundary;
        if (Double.isFinite(headingAccuracyDegrees)
                && (!Double.isFinite(headingAccuracyAnchorDegrees)
                || generationChanged)) {
            headingAccuracyAnchorDegrees = headingAccuracyDegrees;
        }

        boolean usableTier = accuracyTier == ACCURACY_MEDIUM
                || accuracyTier == ACCURACY_HIGH;
        boolean usableConfidence = !hasHeadingAccuracy
                || confidenceBoundary == ConfidenceBoundary.WITHIN;
        return new RotationDecision(true, usableTier && usableConfidence,
                headingDegrees(azimuthRadians), headingAccuracyDegrees,
                accuracyGeneration);
    }

    static double headingDegrees(double azimuthRadians) {
        if (!Double.isFinite(azimuthRadians)) {
            return Double.NaN;
        }
        double result = Math.toDegrees(azimuthRadians) % 360.0;
        if (result < 0.0) {
            result += 360.0;
        }
        return result == -0.0 ? 0.0 : result;
    }

    static boolean canPairGyroscope(long gyroscopeTimestampNanos,
                                    long rotationTimestampNanos) {
        if (gyroscopeTimestampNanos < 0 || rotationTimestampNanos < 0
                || gyroscopeTimestampNanos > rotationTimestampNanos) {
            return false;
        }
        return rotationTimestampNanos - gyroscopeTimestampNanos
                <= MAX_GYROSCOPE_PAIR_AGE_NANOS;
    }

    static double sensorTimestampSeconds(long timestampNanos) {
        return timestampNanos * 1.0e-9;
    }

    static double clockwiseWorldZRateDegreesPerSecond(
            float[] rotationMatrix, float xRadiansPerSecond,
            float yRadiansPerSecond, float zRadiansPerSecond) {
        if (rotationMatrix == null || rotationMatrix.length < 9
                || !Float.isFinite(xRadiansPerSecond)
                || !Float.isFinite(yRadiansPerSecond)
                || !Float.isFinite(zRadiansPerSecond)) {
            return Double.NaN;
        }
        double worldZRadiansPerSecond =
                rotationMatrix[6] * xRadiansPerSecond
                        + rotationMatrix[7] * yRadiansPerSecond
                        + rotationMatrix[8] * zRadiansPerSecond;
        return -Math.toDegrees(worldZRadiansPerSecond);
    }

    static RotationDelivery rotationDelivery(boolean usableAccuracy,
                                               boolean hasGyroscopePair,
                                               boolean newAccuracyGeneration) {
        if (hasGyroscopePair) {
            return RotationDelivery.PAIRED;
        }
        return !usableAccuracy && newAccuracyGeneration
                ? RotationDelivery.CALIBRATION_ONLY
                : RotationDelivery.DROP;
    }

    private RegistrationDecision reconcileRegistration() {
        boolean nextActive = nativeStarted && activityResumed;
        if (nextActive == registrationActive) {
            return new RegistrationDecision(RegistrationAction.NONE, generation);
        }
        registrationActive = nextActive;
        incrementGeneration();
        resetAccuracyTracking();
        return new RegistrationDecision(
                nextActive ? RegistrationAction.REGISTER
                        : RegistrationAction.UNREGISTER,
                generation);
    }

    private static ConfidenceBoundary confidenceBoundary(
            boolean hasHeadingAccuracy, double headingAccuracyDegrees) {
        if (!hasHeadingAccuracy) {
            return ConfidenceBoundary.UNAVAILABLE;
        }
        return Double.isFinite(headingAccuracyDegrees)
                && headingAccuracyDegrees >= 0.0
                && headingAccuracyDegrees <= MAX_HEADING_ACCURACY_DEGREES
                ? ConfidenceBoundary.WITHIN
                : ConfidenceBoundary.OUTSIDE;
    }

    private void resetAccuracyTracking() {
        lastAccuracyTier = -1;
        lastConfidenceBoundary = ConfidenceBoundary.UNAVAILABLE;
        headingAccuracyAnchorDegrees = Double.NaN;
        incrementAccuracyGeneration();
    }

    private void incrementGeneration() {
        generation++;
        if (generation == 0) {
            generation++;
        }
    }

    private void incrementAccuracyGeneration() {
        accuracyGeneration++;
        if (accuracyGeneration == 0) {
            accuracyGeneration++;
        }
    }
}
