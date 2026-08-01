package com.snurhythm.asobmashow;

import android.app.Activity;
import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;

final class AsoBMaShowGyroscopeTurntableManager {
    private final Object lifecycleLock = new Object();
    private final Object stateLock = new Object();
    private final SensorManager sensorManager;
    private final Sensor rotationVectorSensor;
    private final Sensor gyroscopeSensor;
    private final GyroscopeSensorPolicy policy = new GyroscopeSensorPolicy();

    private Registration registration;
    private GyroscopeReading latestGyroscope;
    private long lastDeliveredAccuracyGeneration;
    private boolean destroyed;

    AsoBMaShowGyroscopeTurntableManager(Activity activity) {
        Object service = activity.getSystemService(Context.SENSOR_SERVICE);
        sensorManager = service instanceof SensorManager
                ? (SensorManager) service
                : null;
        rotationVectorSensor = sensorManager == null
                ? null
                : sensorManager.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR);
        gyroscopeSensor = sensorManager == null
                ? null
                : sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
    }

    boolean isSupported() {
        return sensorManager != null
                && rotationVectorSensor != null
                && gyroscopeSensor != null;
    }

    void setNativeStarted(boolean started) {
        synchronized (lifecycleLock) {
            GyroscopeSensorPolicy.RegistrationDecision decision;
            synchronized (stateLock) {
                if (destroyed && started) {
                    return;
                }
                decision = policy.setNativeStarted(started);
            }
            applyDecision(decision);
        }
    }

    void setActivityResumed(boolean resumed) {
        synchronized (lifecycleLock) {
            GyroscopeSensorPolicy.RegistrationDecision decision;
            synchronized (stateLock) {
                if (destroyed && resumed) {
                    return;
                }
                decision = policy.setActivityResumed(resumed);
            }
            applyDecision(decision);
        }
    }

    void destroy() {
        synchronized (lifecycleLock) {
            GyroscopeSensorPolicy.RegistrationDecision resumeDecision;
            GyroscopeSensorPolicy.RegistrationDecision nativeDecision;
            synchronized (stateLock) {
                if (destroyed) {
                    return;
                }
                destroyed = true;
                resumeDecision = policy.setActivityResumed(false);
                nativeDecision = policy.setNativeStarted(false);
            }
            applyDecision(resumeDecision);
            applyDecision(nativeDecision);
            stopRegistration();
        }
    }

    private void applyDecision(GyroscopeSensorPolicy.RegistrationDecision decision) {
        switch (decision.action) {
        case REGISTER:
            startRegistration(decision.generation);
            break;
        case UNREGISTER:
            stopRegistration();
            break;
        case NONE:
            break;
        }
    }

    private void startRegistration(long generation) {
        if (!isSupported()) {
            failPolicyRegistration(generation);
            notifyRegistrationResult(generation, false);
            return;
        }

        HandlerThread thread = new HandlerThread(
                "AsoBMaShow-gyroscope-" + generation);
        try {
            thread.start();
        } catch (RuntimeException exception) {
            failPolicyRegistration(generation);
            notifyRegistrationResult(generation, false);
            return;
        }

        GenerationListener listener = new GenerationListener(generation);
        Registration candidate = new Registration(
                generation, thread, new Handler(thread.getLooper()), listener);
        synchronized (stateLock) {
            if (destroyed || !policy.acceptsCallback(generation)) {
                shutdownThread(thread);
                return;
            }
            registration = candidate;
            latestGyroscope = null;
            lastDeliveredAccuracyGeneration = 0;
        }

        boolean rotationRegistered = false;
        boolean gyroscopeRegistered = false;
        try {
            rotationRegistered = sensorManager.registerListener(
                    listener, rotationVectorSensor, SensorManager.SENSOR_DELAY_GAME,
                    candidate.handler);
            if (rotationRegistered) {
                gyroscopeRegistered = sensorManager.registerListener(
                        listener, gyroscopeSensor, SensorManager.SENSOR_DELAY_GAME,
                        candidate.handler);
            }
        } catch (RuntimeException ignored) {
            // Registration failure is reported to the native retry supervisor.
        }

        if (!rotationRegistered || !gyroscopeRegistered) {
            detachRegistration(candidate);
            sensorManager.unregisterListener(listener);
            shutdownThread(thread);
            failPolicyRegistration(generation);
            notifyRegistrationResult(generation, false);
            return;
        }

        notifyRegistrationResult(generation, true);
    }

    private void stopRegistration() {
        Registration stopped;
        synchronized (stateLock) {
            stopped = registration;
            registration = null;
            latestGyroscope = null;
            lastDeliveredAccuracyGeneration = 0;
        }
        if (stopped == null) {
            return;
        }
        try {
            sensorManager.unregisterListener(stopped.listener);
        } catch (RuntimeException ignored) {
            // The sensor service may already be shutting down with the activity.
        }
        shutdownThread(stopped.thread);
    }

    private void detachRegistration(Registration candidate) {
        synchronized (stateLock) {
            if (registration == candidate) {
                registration = null;
                latestGyroscope = null;
                lastDeliveredAccuracyGeneration = 0;
            }
        }
    }

    private void failPolicyRegistration(long generation) {
        synchronized (stateLock) {
            policy.registrationFailed(generation);
        }
    }

    private static void shutdownThread(HandlerThread thread) {
        thread.quitSafely();
        if (Thread.currentThread() == thread) {
            return;
        }
        try {
            thread.join();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    private void handleSensorEvent(long generation, SensorEvent event) {
        if (event == null || event.sensor == null) {
            return;
        }
        if (event.sensor == gyroscopeSensor) {
            handleGyroscope(generation, event);
        } else if (event.sensor == rotationVectorSensor) {
            handleRotationVector(generation, event);
        }
    }

    private void handleGyroscope(long generation, SensorEvent event) {
        if (event.values == null || event.values.length < 3) {
            return;
        }
        synchronized (stateLock) {
            if (!policy.acceptsCallback(generation)) {
                return;
            }
            latestGyroscope = new GyroscopeReading(
                    event.timestamp, event.values[0], event.values[1], event.values[2]);
        }
    }

    private void handleRotationVector(long generation, SensorEvent event) {
        if (event.values == null || event.values.length < 3) {
            return;
        }

        GyroscopeReading gyroscope;
        synchronized (stateLock) {
            if (!policy.acceptsCallback(generation)) {
                return;
            }
            gyroscope = latestGyroscope;
        }
        boolean hasGyroscopePair = gyroscope != null
                && GyroscopeSensorPolicy.canPairGyroscope(
                        gyroscope.timestampNanos, event.timestamp);

        float[] rotationMatrix = new float[9];
        float[] orientation = new float[3];
        try {
            SensorManager.getRotationMatrixFromVector(rotationMatrix, event.values);
            SensorManager.getOrientation(rotationMatrix, orientation);
        } catch (RuntimeException ignored) {
            return;
        }

        boolean hasHeadingAccuracy = event.values.length > 4;
        double headingAccuracyRadians = hasHeadingAccuracy
                ? event.values[4]
                : 0.0;
        GyroscopeSensorPolicy.RotationDecision decision;
        GyroscopeSensorPolicy.RotationDelivery delivery;
        synchronized (stateLock) {
            decision = policy.evaluateRotationVector(
                    generation, event.accuracy, orientation[0],
                    hasHeadingAccuracy, headingAccuracyRadians);
            boolean newAccuracyGeneration = decision.accuracyGeneration
                    != lastDeliveredAccuracyGeneration;
            delivery = GyroscopeSensorPolicy.rotationDelivery(
                    decision.usableAccuracy, hasGyroscopePair,
                    newAccuracyGeneration);
            if (decision.callbackAccepted
                    && delivery != GyroscopeSensorPolicy.RotationDelivery.DROP) {
                lastDeliveredAccuracyGeneration = decision.accuracyGeneration;
            }
        }
        if (!decision.callbackAccepted) {
            return;
        }
        if (delivery == GyroscopeSensorPolicy.RotationDelivery.DROP) {
            return;
        }

        double clockwiseRateDegreesPerSecond = delivery
                == GyroscopeSensorPolicy.RotationDelivery.PAIRED
                ? GyroscopeSensorPolicy.clockwiseWorldZRateDegreesPerSecond(
                        rotationMatrix, gyroscope.xRadiansPerSecond,
                        gyroscope.yRadiansPerSecond,
                        gyroscope.zRadiansPerSecond)
                : 0.0;
        boolean discontinuity = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                && event.firstEventAfterDiscontinuity;
        notifySample(generation, decision.headingDegrees,
                clockwiseRateDegreesPerSecond,
                GyroscopeSensorPolicy.sensorTimestampSeconds(event.timestamp),
                decision.accuracyGeneration, decision.usableAccuracy,
                discontinuity);
    }

    private static void notifyRegistrationResult(long generation, boolean success) {
        try {
            nativeGyroscopeRegistrationResult(generation, success);
        } catch (UnsatisfiedLinkError ignored) {
            // The activity can outlive native teardown during process shutdown.
        }
    }

    private static void notifySample(long generation, double headingDegrees,
                                     double clockwiseRateDegreesPerSecond,
                                     double sensorTimestampSeconds,
                                     long accuracyGeneration,
                                     boolean usableAccuracy,
                                     boolean discontinuity) {
        try {
            nativeGyroscopeSample(generation, headingDegrees,
                    clockwiseRateDegreesPerSecond, sensorTimestampSeconds,
                    accuracyGeneration, usableAccuracy, discontinuity);
        } catch (UnsatisfiedLinkError ignored) {
            // The activity can outlive native teardown during process shutdown.
        }
    }

    private static native void nativeGyroscopeRegistrationResult(
            long generation, boolean success);

    private static native void nativeGyroscopeSample(
            long generation, double headingDegrees,
            double clockwiseRateDegreesPerSecond, double sensorTimestampSeconds,
            long accuracyGeneration, boolean usableAccuracy,
            boolean discontinuity);

    private final class GenerationListener implements SensorEventListener {
        private final long generation;

        GenerationListener(long generation) {
            this.generation = generation;
        }

        @Override
        public void onSensorChanged(SensorEvent event) {
            handleSensorEvent(generation, event);
        }

        @Override
        public void onAccuracyChanged(Sensor sensor, int accuracy) {
            // Accuracy is consumed from each rotation-vector SensorEvent.
        }
    }

    private static final class Registration {
        final long generation;
        final HandlerThread thread;
        final Handler handler;
        final GenerationListener listener;

        Registration(long generation, HandlerThread thread, Handler handler,
                     GenerationListener listener) {
            this.generation = generation;
            this.thread = thread;
            this.handler = handler;
            this.listener = listener;
        }
    }

    private static final class GyroscopeReading {
        final long timestampNanos;
        final float xRadiansPerSecond;
        final float yRadiansPerSecond;
        final float zRadiansPerSecond;

        GyroscopeReading(long timestampNanos, float xRadiansPerSecond,
                         float yRadiansPerSecond, float zRadiansPerSecond) {
            this.timestampNanos = timestampNanos;
            this.xRadiansPerSecond = xRadiansPerSecond;
            this.yRadiansPerSecond = yRadiansPerSecond;
            this.zRadiansPerSecond = zRadiansPerSecond;
        }
    }
}
