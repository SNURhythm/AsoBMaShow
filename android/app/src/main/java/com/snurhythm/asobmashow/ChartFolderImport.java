package com.snurhythm.asobmashow;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

final class ChartFolderImport {
    static final class Entry {
        final String id;
        final String name;
        final boolean directory;
        final long size;
        final long lastModified;

        Entry(String id, String name, boolean directory, long size, long lastModified) {
            this.id = id;
            this.name = name;
            this.directory = directory;
            this.size = size;
            this.lastModified = lastModified;
        }
    }

    interface Source {
        Entry root() throws IOException;
        List<Entry> children(Entry directory) throws IOException;
        InputStream open(Entry file) throws IOException;
        // Must not pause between the engine's final validation and the provider call.
        void delete(Entry directory) throws IOException;
    }

    enum Phase { COUNTING, COPYING, REMOVING_SOURCE }

    interface Progress {
        void update(int copiedFiles, int totalFiles, long copiedBytes, long totalBytes,
                    String currentName, Phase phase);
    }

    static final class Result {
        final boolean complete;
        final boolean retainedOutput;
        final String error;

        Result(boolean complete, boolean retainedOutput, String error) {
            this.complete = complete;
            this.retainedOutput = retainedOutput;
            this.error = error;
        }
    }

    private static final class PlannedEntry {
        final Entry entry;
        final String destinationName;
        final List<PlannedEntry> children;

        PlannedEntry(Entry entry, String destinationName, List<PlannedEntry> children) {
            this.entry = entry;
            this.destinationName = destinationName;
            this.children = Collections.unmodifiableList(new ArrayList<>(children));
        }
    }

    static Result run(Source source, File output, boolean move, ChartImportCopyControl control,
                      Progress progress, Runnable beforeSourceDeletion) {
        return new Transfer(source, output, move, control, progress, beforeSourceDeletion).run();
    }

    private static final class Transfer {
        private final Source source;
        private final File requestedOutput;
        private final boolean move;
        private final ChartImportCopyControl control;
        private final Progress progress;
        private final Runnable beforeSourceDeletion;
        private final Set<String> discoveredIds = new HashSet<>();
        private File output;
        private boolean ownsOutput;
        private boolean deletionAttempted;
        private int totalFiles;
        private int copiedFiles;
        private long totalBytes;
        private long copiedBytes;

        Transfer(Source source, File output, boolean move, ChartImportCopyControl control,
                 Progress progress, Runnable beforeSourceDeletion) {
            this.source = source;
            this.requestedOutput = output;
            this.move = move;
            this.control = control;
            this.progress = progress;
            this.beforeSourceDeletion = beforeSourceDeletion;
        }

        Result run() {
            try {
                report("", Phase.COUNTING);
                control.checkpoint();
                Entry root = source.root();
                if (root == null || !root.directory) {
                    throw new IOException("The selected source is not a folder.");
                }
                PlannedEntry plan = discover(root, root.name, 0);
                control.checkpoint();
                output = requestedOutput.getCanonicalFile();
                if (!output.mkdir()) {
                    throw new IOException("Cannot create a new destination folder: " + output.getName());
                }
                ownsOutput = true;
                copyDirectory(plan, output);
                control.checkpoint();
                report(root.name, Phase.COPYING);
                return new Result(true, true, "");
            } catch (IOException | RuntimeException error) {
                // A provider can remove part of a directory and then throw. From the first
                // attempted deletion onward, destination files may be the only copies left.
                if (ownsOutput && !deletionAttempted) {
                    removeOwnedOutput(output);
                }
                boolean retained = ownsOutput && output.exists();
                String message = error.getMessage();
                if (message == null || message.isEmpty()) message = error.getClass().getSimpleName();
                return new Result(false, retained, message);
            }
        }

        private PlannedEntry discover(Entry entry, String destinationName, int depth) throws IOException {
            control.checkpoint();
            validate(entry);
            if (depth > 256 || !discoveredIds.add(entry.id)) {
                throw new IOException("Source contains a repeated entry or excessively deep folders.");
            }
            List<PlannedEntry> children = new ArrayList<>();
            if (entry.directory) {
                Set<String> names = new HashSet<>();
                for (Entry child : list(entry)) {
                    validate(child);
                    children.add(discover(child, uniqueName(child.name, names), depth + 1));
                }
            } else {
                // Admit the entire move before deleting any completed subtree. Without
                // both fields, a later edit can be indistinguishable from the copied file.
                if (move && (entry.size < 0 || entry.lastModified <= 0)) {
                    throw new IOException("Cannot safely move a file without its size and modification time: "
                            + entry.name + ". Use Copy instead.");
                }
                if (totalFiles == Integer.MAX_VALUE) throw new IOException("Too many source files.");
                totalFiles++;
                if (entry.size < 0) totalBytes = -1;
                else if (totalBytes >= 0) {
                    if (entry.size > Long.MAX_VALUE - totalBytes) throw new IOException("Source is too large.");
                    totalBytes += entry.size;
                }
            }
            report(entry.name, Phase.COUNTING);
            return new PlannedEntry(entry, destinationName, children);
        }

        private List<Entry> list(Entry directory) throws IOException {
            control.checkpoint();
            List<Entry> children = source.children(directory);
            if (children == null) throw new IOException("Cannot list source folder: " + directory.name);
            return new ArrayList<>(children);
        }

        private void copyDirectory(PlannedEntry directory, File destination) throws IOException {
            control.checkpoint();
            // Release each complete subtree before opening the next one. Parent-level
            // files follow its subfolders so a large root does not delay that recovery.
            for (PlannedEntry child : directory.children) {
                if (!child.entry.directory) continue;
                control.checkpoint();
                File childOutput = childPath(destination, child.destinationName);
                if (!childOutput.mkdir()) throw new IOException("Cannot create folder: " + child.destinationName);
                copyDirectory(child, childOutput);
            }
            for (PlannedEntry child : directory.children) {
                if (!child.entry.directory) copyFile(child, childPath(destination, child.destinationName));
            }
            if (move) {
                report(directory.entry.name, Phase.REMOVING_SOURCE);
                control.checkpoint();
                beforeSourceDeletion.run();
                while (true) {
                    control.checkpoint();
                    long generation = control.pauseGeneration();
                    verifyRemainingSource(directory);
                    if (control.continueWithoutWaiting(generation)) break;
                }
                deletionAttempted = true;
                source.delete(directory.entry);
            }
        }

        private void copyFile(PlannedEntry file, File destination) throws IOException {
            control.checkpoint();
            report(file.entry.name, Phase.COPYING);
            if (!destination.createNewFile()) throw new IOException("Destination already exists: " + destination.getName());
            boolean complete = false;
            long startBytes = copiedBytes;
            try {
                try (InputStream input = source.open(file.entry);
                     FileOutputStream stream = new FileOutputStream(destination)) {
                    if (input == null) throw new IOException("Cannot read source file: " + file.entry.name);
                    control.copy(input, stream, count -> {
                        copiedBytes = Math.addExact(copiedBytes, count);
                        report(file.entry.name, Phase.COPYING);
                    });
                    control.checkpoint();
                    if (file.entry.size >= 0 && copiedBytes - startBytes != file.entry.size) {
                        throw new IOException("Source file size changed: " + file.entry.name);
                    }
                    if (move) stream.getFD().sync();
                }
                complete = true;
                copiedFiles++;
                report(file.entry.name, Phase.COPYING);
            } finally {
                if (!complete && !destination.delete() && destination.exists()) {
                    throw new IOException("Cannot remove incomplete destination file: " + destination.getName());
                }
            }
        }

        private void verifyRemainingSource(PlannedEntry directory) throws IOException {
            Map<String, Entry> remainingFiles = new HashMap<>();
            for (PlannedEntry child : directory.children) {
                if (!child.entry.directory) remainingFiles.put(child.entry.id, child.entry);
            }
            for (Entry current : list(directory.entry)) {
                control.checkpoint();
                validate(current);
                Entry original = remainingFiles.remove(current.id);
                // Completed child directories have already been removed. A surviving or
                // newly added directory must never be recursively deleted by the parent.
                if (original == null || current.directory || !original.name.equals(current.name)
                        || original.size != current.size
                        || original.lastModified != current.lastModified) {
                    throw new IOException("Source folder changed while copying: " + directory.entry.name);
                }
            }
            if (!remainingFiles.isEmpty()) {
                throw new IOException("Source folder changed while copying: " + directory.entry.name);
            }
        }

        private File childPath(File parent, String name) throws IOException {
            File child = new File(parent, name);
            if (!child.getCanonicalFile().getParentFile().equals(parent)) {
                throw new IOException("Unsafe destination path: " + name);
            }
            return child;
        }

        private void report(String name, Phase phase) {
            progress.update(copiedFiles, totalFiles, copiedBytes, totalBytes, name, phase);
        }
    }

    private static void validate(Entry entry) throws IOException {
        if (entry == null || entry.id == null || entry.id.isEmpty() || entry.name == null
                || entry.name.isEmpty() || entry.name.equals(".") || entry.name.equals("..")
                || entry.name.indexOf('/') >= 0 || entry.name.indexOf('\\') >= 0
                || entry.size < -1 || entry.lastModified < -1) {
            throw new IOException("Source contains an invalid file or folder name.");
        }
        for (int index = 0; index < entry.name.length(); index++) {
            if (Character.isISOControl(entry.name.charAt(index))) {
                throw new IOException("Source contains an invalid file or folder name.");
            }
        }
    }

    private static String uniqueName(String name, Set<String> names) {
        String candidate = name;
        int suffix = 2;
        int dot = name.lastIndexOf('.');
        if (dot <= 0) dot = name.length();
        while (!names.add(candidate.toLowerCase(Locale.ROOT))) {
            candidate = name.substring(0, dot) + " (" + suffix++ + ")" + name.substring(dot);
        }
        return candidate;
    }

    private static void removeOwnedOutput(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                try {
                    // Do not traverse a symlink installed by another process during cleanup.
                    if (child.getCanonicalFile().equals(child.getAbsoluteFile())) removeOwnedOutput(child);
                    else child.delete();
                } catch (IOException ignored) {
                    child.delete();
                }
            }
        }
        file.delete();
    }
}
