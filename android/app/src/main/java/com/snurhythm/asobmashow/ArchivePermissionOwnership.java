package com.snurhythm.asobmashow;

import java.util.HashSet;
import java.util.Set;

/** Durable ownership is separate from Android's list of granted URIs. */
final class ArchivePermissionOwnership {
    interface Store {
        Set<String> read();
        boolean write(Set<String> uris);
    }
    interface Release { void run() throws Exception; }

    static synchronized boolean record(Store store, String uri) {
        Set<String> owned = new HashSet<>(store.read());
        owned.add(uri);
        return store.write(owned);
    }

    // Native registration serializes this with new references, and calls only
    // when no reference (including an in-progress import) still needs the URI.
    static synchronized boolean release(Store store, String uri, Release release) {
        Set<String> owned = new HashSet<>(store.read());
        if (!owned.contains(uri)) return true;
        try { release.run(); }
        catch (Exception error) { return false; }
        owned.remove(uri);
        return store.write(owned);
    }
}
