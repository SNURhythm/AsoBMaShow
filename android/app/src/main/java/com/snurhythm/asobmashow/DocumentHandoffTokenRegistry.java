package com.snurhythm.asobmashow;

import java.util.HashMap;
import java.util.Map;

/** Tracks pre-registered, never-reused native document operation tokens. */
final class DocumentHandoffTokenRegistry {
    enum Activation {
        ACTIVE,
        CANCELLED,
        UNKNOWN
    }

    private enum State {
        PENDING,
        ACTIVE,
        CANCELLED,
        FINISHED
    }

    private final Map<String, State> states = new HashMap<>();

    synchronized boolean register(String token) {
        if (token == null || token.isEmpty() || states.containsKey(token)) {
            return false;
        }
        states.put(token, State.PENDING);
        return true;
    }

    synchronized Activation activate(String token) {
        State state = states.get(token);
        if (state == null || state == State.FINISHED) {
            return Activation.UNKNOWN;
        }
        if (state == State.CANCELLED) {
            return Activation.CANCELLED;
        }
        if (state != State.PENDING) {
            return Activation.UNKNOWN;
        }
        states.put(token, State.ACTIVE);
        return Activation.ACTIVE;
    }

    synchronized boolean cancel(String token) {
        State state = states.get(token);
        if (state == null || state == State.FINISHED) {
            return false;
        }
        states.put(token, State.CANCELLED);
        return true;
    }

    synchronized void finish(String token) {
        if (states.containsKey(token)) {
            states.put(token, State.FINISHED);
        }
    }

    synchronized void retire(String token) {
        states.remove(token);
    }

    synchronized int sizeForTesting() {
        return states.size();
    }
}
