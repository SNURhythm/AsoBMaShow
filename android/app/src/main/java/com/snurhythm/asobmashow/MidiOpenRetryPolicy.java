package com.snurhythm.asobmashow;

import java.util.HashMap;
import java.util.Map;

final class MidiOpenRetryPolicy {
    static final int MAX_ATTEMPTS = 3;

    private final Map<Integer, Integer> attempts = new HashMap<>();

    boolean beginAttempt(int deviceId) {
        int attempted = attempts.containsKey(deviceId) ? attempts.get(deviceId) : 0;
        if (attempted >= MAX_ATTEMPTS) {
            return false;
        }
        attempts.put(deviceId, attempted + 1);
        return true;
    }

    void reset(int deviceId) {
        attempts.remove(deviceId);
    }

    void remove(int deviceId) {
        attempts.remove(deviceId);
    }

    void clear() {
        attempts.clear();
    }
}
