package com.snurhythm.asobmashow;

import java.util.function.Consumer;

/** Coalesce a file-manager copy; persist only the clean/dirty transition. */
final class DocumentsLibraryChanges {
    private final Consumer<Boolean> persist;
    private boolean dirty;
    private int writers;
    private long revision;
    private long lastChange;

    DocumentsLibraryChanges(boolean pending, Consumer<Boolean> persist) {
        this.persist = persist;
        dirty = pending;
        revision = pending ? 1 : 0;
    }

    synchronized void changed(long now) {
        ++revision;
        lastChange = now;
        if (!dirty) {
            dirty = true;
            persist.accept(true);
        }
    }

    synchronized void writerOpened(long now, boolean affectsBms) {
        ++writers;
        if (affectsBms) changed(now);
    }

    synchronized void writerClosed(long now) {
        --writers;
        if (dirty) changed(now);
    }

    synchronized long readyRevision(long now) {
        return dirty && writers == 0 && now - lastChange >= 2000 ? revision : 0;
    }

    synchronized void acknowledge(long completedRevision) {
        if (dirty && completedRevision != 0 && revision == completedRevision && writers == 0) {
            dirty = false;
            persist.accept(false);
        }
    }
}
