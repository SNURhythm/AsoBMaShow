package com.snurhythm.asobmashow;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.util.ArrayList;

/** Coordinates document writers, imports, and native storage maintenance. */
final class DocumentsMutationGuard {
    // Callers hold this monitor across the guard check and actual filesystem
    // mutation. A reservation stays active while a long-running import releases
    // the monitor, allowing mutations in unrelated document subtrees.
    static final Object LOCK = new Object();
    private static final ArrayList<Reservation> reservations = new ArrayList<>();
    private static final ArrayList<Reservation> writers = new ArrayList<>();

    static final class Reservation implements AutoCloseable {
        File canonicalOutput;

        private Reservation(File output) { canonicalOutput = output; }

        @Override public void close() {
            synchronized (LOCK) {
                // Remove by lease identity: closing an old lease twice must not
                // release a later reservation for the same destination.
                reservations.remove(this);
                writers.remove(this);
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

    static Reservation reserveWriter(File file) throws IOException {
        synchronized (LOCK) {
            File canonical = file.getCanonicalFile();
            requireUnreservedLocked(canonical.toPath(), false);
            Reservation lease = new Reservation(canonical);
            writers.add(lease);
            return lease;
        }
    }

    // Ordinary provider renames remain supported while a writer is open.
    // Move its lease with the subtree so maintenance cannot miss the open FD.
    static void rename(File source, File destination) throws IOException {
        synchronized (LOCK) {
            Path from = source.toPath();
            Path to = destination.toPath();
            ArrayList<Reservation> affected = new ArrayList<>();
            for (Reservation writer : writers) {
                Path path = writer.canonicalOutput.toPath();
                if (isSameOrDescendant(path, from)) {
                    Path prefix = path;
                    while (prefix.getNameCount() > from.getNameCount()) prefix = prefix.getParent();
                    // Case aliases can also be distinct siblings. Identify the
                    // actual subtree while both paths still exist, before moving.
                    if (Files.isSameFile(prefix, from)) affected.add(writer);
                }
            }
            Files.move(from, to);
            for (Reservation writer : affected) {
                Path path = writer.canonicalOutput.toPath();
                // Relativize compares case-sensitive components and can produce
                // ".." for an alias. Keep only the already identified suffix.
                int prefixLength = from.getNameCount();
                writer.canonicalOutput = path.getNameCount() == prefixLength
                        ? to.toFile()
                        : to.resolve(path.subpath(prefixLength, path.getNameCount())).toFile();
            }
        }
    }

    static final class ReservationGroup implements AutoCloseable {
        private final ArrayList<Reservation> leases = new ArrayList<>();
        @Override public void close() {
            synchronized (LOCK) {
                for (Reservation lease : leases) lease.close();
                leases.clear();
            }
        }
    }

    static ReservationGroup reserveExistingSubtrees(String[] paths) throws IOException {
        synchronized (LOCK) {
            ReservationGroup group = new ReservationGroup();
            try {
                for (String value : paths) {
                    Path path = new File(value).toPath().toAbsolutePath().normalize();
                    for (Path part = path; part != null; part = part.getParent()) {
                        if (Files.isSymbolicLink(part)) throw new IOException("Storage folder is a symbolic link");
                    }
                    if (!Files.isDirectory(path, LinkOption.NOFOLLOW_LINKS)
                            && !Files.isRegularFile(path, LinkOption.NOFOLLOW_LINKS))
                        throw new IOException("Storage folder or archive is unavailable");
                    File canonical = path.toFile().getCanonicalFile();
                    requireUnreservedLocked(canonical.toPath(), true);
                    for (Reservation writer : writers) {
                        if (isSameOrDescendant(writer.canonicalOutput.toPath(), canonical.toPath()))
                            throw new IOException("Cannot modify documents: import in progress or document writer open");
                    }
                    Reservation lease = new Reservation(canonical);
                    reservations.add(lease);
                    group.leases.add(lease);
                }
                return group;
            } catch (IOException | RuntimeException error) {
                group.close();
                throw error;
            }
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
