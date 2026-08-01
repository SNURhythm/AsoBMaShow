package com.snurhythm.asobmashow;

import java.util.HashMap;
import java.util.Map;

final class MidiOpenRetryPolicy {
    static final int MAX_ATTEMPTS = 3;

    private final Map<Integer, Integer> attempts = new HashMap<>();
    private final Map<Integer, Long> scheduledRetries = new HashMap<>();
    private long nextRetryToken = 1;

    boolean beginAttempt(int deviceId) {
        int attempted = attempts.containsKey(deviceId) ? attempts.get(deviceId) : 0;
        if (attempted >= MAX_ATTEMPTS) {
            return false;
        }
        attempts.put(deviceId, attempted + 1);
        return true;
    }

    long scheduleRetry(int deviceId) {
        if (scheduledRetries.containsKey(deviceId)) {
            return 0;
        }
        long token = nextRetryToken++;
        if (token == 0) {
            token = nextRetryToken++;
        }
        scheduledRetries.put(deviceId, token);
        return token;
    }

    boolean isRetryScheduled(int deviceId) {
        return scheduledRetries.containsKey(deviceId);
    }

    boolean beginScheduledAttempt(int deviceId, long token) {
        Long scheduledToken = scheduledRetries.get(deviceId);
        if (scheduledToken == null || scheduledToken != token) {
            return false;
        }
        scheduledRetries.remove(deviceId);
        return beginAttempt(deviceId);
    }

    void reset(int deviceId) {
        attempts.remove(deviceId);
    }

    void remove(int deviceId) {
        attempts.remove(deviceId);
        scheduledRetries.remove(deviceId);
    }

    void clear() {
        attempts.clear();
        scheduledRetries.clear();
    }
}
