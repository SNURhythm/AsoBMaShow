package com.snurhythm.asobmashow;

import android.os.CancellationSignal;
import android.os.OperationCanceledException;
import java.util.HashSet;
import java.util.Set;

/** ContentResolver binds one remote operation to each signal, so never share them. */
final class SafImportCancellation {
    private final Set<Operation> active = new HashSet<>();
    private boolean cancelled;

    final class Operation implements AutoCloseable {
        final CancellationSignal signal = new CancellationSignal();
        @Override public void close() {
            synchronized (SafImportCancellation.this) { active.remove(this); }
        }
    }

    synchronized Operation begin() {
        if (cancelled) throw new OperationCanceledException();
        Operation operation = new Operation();
        active.add(operation);
        return operation;
    }

    void cancel() {
        Operation[] pending;
        synchronized (this) {
            cancelled = true;
            pending = active.toArray(new Operation[0]);
        }
        // Never hold the registry monitor across provider callbacks.
        RuntimeException failure = null;
        for (Operation operation : pending) {
            try { operation.signal.cancel(); }
            catch (RuntimeException error) { failure = error; }
        }
        if (failure != null) throw failure;
    }
}
