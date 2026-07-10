package com.snurhythm.asobmashow;

import java.util.concurrent.atomic.AtomicLong;

/** Allocates activity request codes once and saturates permanently. */
final class DocumentHandoffRequestCodeAllocator {
    private final int first;
    private final int last;
    private final AtomicLong next;

    DocumentHandoffRequestCodeAllocator(int first, int last) {
        if (first < 0 || last < first) {
            throw new IllegalArgumentException("Invalid request code range.");
        }
        this.first = first;
        this.last = last;
        next = new AtomicLong(first);
    }

    int allocate() {
        while (true) {
            long current = next.get();
            if (current > last) {
                return -1;
            }
            if (next.compareAndSet(current, current + 1)) {
                return (int) current;
            }
        }
    }

    boolean wasIssued(int requestCode) {
        long upperExclusive = Math.min(next.get(), (long) last + 1L);
        return requestCode >= first && requestCode <= last &&
                requestCode < upperExclusive;
    }
}
