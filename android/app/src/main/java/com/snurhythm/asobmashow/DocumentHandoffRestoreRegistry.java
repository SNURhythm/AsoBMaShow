package com.snurhythm.asobmashow;

import java.util.HashMap;
import java.util.Map;
import java.util.function.BooleanSupplier;

/** Serializes every write and rollback for the same provider URI. */
final class DocumentHandoffRestoreRegistry {
    static final class Ticket {
        final String key;

        Ticket(String key) {
            this.key = key;
        }
    }

    private final Map<String, Ticket> pending = new HashMap<>();

    synchronized Ticket acquire(String key) throws InterruptedException {
        return acquire(key, () -> false);
    }

    synchronized Ticket acquire(String key, BooleanSupplier cancelled)
            throws InterruptedException {
        while (pending.containsKey(key)) {
            if (cancelled.getAsBoolean()) {
                return null;
            }
            wait(25);
        }
        if (cancelled.getAsBoolean()) {
            return null;
        }
        Ticket ticket = new Ticket(key);
        pending.put(key, ticket);
        return ticket;
    }

    void complete(Ticket ticket) {
        synchronized (this) {
            if (pending.get(ticket.key) == ticket) {
                pending.remove(ticket.key);
                notifyAll();
            }
        }
    }
}
