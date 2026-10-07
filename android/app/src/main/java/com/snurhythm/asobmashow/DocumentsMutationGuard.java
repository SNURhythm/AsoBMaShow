package com.snurhythm.asobmashow;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.util.ArrayList;

/** Coordinates document mutations with imports that own a new destination. */
final class DocumentsMutationGuard {
    // Callers hold this monitor across the guard check and actual filesystem
    // mutation. A reservation stays active while a long-running import releases
    // the monitor, allowing mutations in unrelated document subtrees.
    static final Object LOCK = new Object();
    private static final ArrayList<Reservation> reservations = new ArrayList<>();

    static final class Reservation implements AutoCloseable {
        final File canonicalOutput;

        private Reservation(File output) { canonicalOutput = output; }

        @Override public void close() {
            synchronized (LOCK) {
                // Remove by lease identity: closing an old lease twice must not
                // release a later reservation for the same destination.
                reservations.remove(this);
            }
        }
    }

    static Reservation reserveNewDestination(File output) throws IOException {
        synchronized (LOCK) {
            File canonicalOutput = output.getCanonicalFile();
            requireUnreservedLocked(canonicalOutput.toPath(), true);
            // Check the original path as well: canonicalization can follow a
            // final symlink. NOFOLLOW also rejects dangling symlinks, and
            // notExists fails closed when absence cannot be established.
            if (!Files.notExists(output.toPath(), LinkOption.NOFOLLOW_LINKS)
                    || !Files.notExists(canonicalOutput.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                throw new IOException("Import destination must be new and accessible: " + output);
            }
            Reservation reservation = new Reservation(canonicalOutput);
            reservations.add(reservation);
            return reservation;
        }
    }

    static void requireUnreserved(File path, boolean recursive) throws IOException {
        synchronized (LOCK) {
            requireUnreservedLocked(path.getCanonicalFile().toPath(), recursive);
        }
    }

    private static void requireUnreservedLocked(Path path, boolean recursive) throws IOException {
        for (Reservation reservation : reservations) {
            Path reserved = reservation.canonicalOutput.toPath();
            if (isSameOrDescendant(path, reserved)
                    || (recursive && isSameOrDescendant(reserved, path))) {
                throw new IOException("Cannot modify documents: import in progress at " + reserved);
            }
        }
    }

    private static boolean isSameOrDescendant(Path path, Path ancestor) {
        // External storage may fold case even when Java's Path comparison does
        // not. Conservatively reserve case aliases on every filesystem, while
        // retaining component boundaries so "chart-extra" remains a sibling.
        if (path.getNameCount() < ancestor.getNameCount()
                || !path.getRoot().toString().equalsIgnoreCase(ancestor.getRoot().toString())) {
            return false;
        }
        for (int index = 0; index < ancestor.getNameCount(); ++index) {
            if (!path.getName(index).toString().equalsIgnoreCase(
                    ancestor.getName(index).toString())) return false;
        }
        return true;
    }

    private DocumentsMutationGuard() {}
}
