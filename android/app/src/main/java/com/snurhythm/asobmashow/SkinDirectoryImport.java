package com.snurhythm.asobmashow;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/** Copies provider content without renaming paths that skin scripts may reference. */
final class SkinDirectoryImport {
    static final class Entry {
        final String id, name;
        final boolean directory;
        final long size;
        Entry(String id, String name, boolean directory, long size) {
            this.id = id; this.name = name; this.directory = directory; this.size = size;
        }
    }
    interface Source {
        Entry root() throws IOException;
        List<Entry> children(Entry directory, long remaining) throws IOException;
        InputStream open(Entry file) throws IOException;
        void checkpoint() throws IOException;
    }
    static final class Limits {
        final long bytes, entries, depth, pathBytes, fileBytes;
        Limits(long bytes, long entries, long depth, long pathBytes, long fileBytes) {
            this.bytes = bytes; this.entries = entries; this.depth = depth;
            this.pathBytes = pathBytes; this.fileBytes = fileBytes;
        }
    }
    private final Source source;
    private final Limits limits;
    private final Set<String> seen = new HashSet<>();
    private long entries, bytes;
    private SkinDirectoryImport(Source source, Limits limits) { this.source = source; this.limits = limits; }

    static String copy(Source source, Path output, Limits limits) throws IOException {
        if (limits.bytes <= 0 || limits.entries <= 0 || limits.depth <= 0 ||
                limits.pathBytes <= 0 || limits.fileBytes <= 0) throw new IOException("Invalid folder import limits.");
        // CREATE_NEW semantics: never remove an existing path on failure.
        Files.createDirectory(output);
        try {
            source.checkpoint();
            Entry root = source.root();
            if (root == null || !root.directory) throw new IOException("Select a folder to import.");
            requireName(root.name);
            SkinDirectoryImport copy = new SkinDirectoryImport(source, limits);
            copy.requireUnique(root);
            copy.copyChildren(root, output, "", 1);
            source.checkpoint();
            return root.name;
        } catch (IOException | RuntimeException e) {
            try { removeTree(output); } catch (IOException cleanup) { e.addSuppressed(cleanup); }
            throw e;
        }
    }
    private void requireUnique(Entry entry) throws IOException {
        if (entry.id == null || entry.id.isEmpty() || !seen.add(entry.id))
            throw new IOException("Folder contains repeated or cyclic documents.");
    }
    private void copyChildren(Entry directory, Path destination, String relative, long depth) throws IOException {
        source.checkpoint();
        List<Entry> children = source.children(directory, limits.entries - entries);
        if (children.size() > limits.entries - entries)
            throw new IOException("Folder exceeds the skin entry limit.");
        entries += children.size();
        for (Entry child : children) {
            source.checkpoint();
            if (depth > limits.depth)
                throw new IOException("Folder exceeds the skin entry or depth limit.");
            requireUnique(child);
            requireName(child.name);
            String path = relative.isEmpty() ? child.name : relative + "/" + child.name;
            if (path.getBytes(StandardCharsets.UTF_8).length > limits.pathBytes)
                throw new IOException("Folder path exceeds the skin path limit.");
            Path output = destination.resolve(child.name);
            if (child.directory) {
                Files.createDirectory(output);
                copyChildren(child, output, path, depth + 1);
            } else {
                if (child.size > limits.fileBytes || child.size > limits.bytes - bytes)
                    throw new IOException("Folder exceeds the skin size limit.");
                long fileBytes = 0;
                try (InputStream input = source.open(child);
                     OutputStream stream = Files.newOutputStream(output, StandardOpenOption.CREATE_NEW)) {
                    if (input == null) throw new IOException("Could not read a skin file.");
                    byte[] buffer = new byte[64 * 1024];
                    while (true) {
                        source.checkpoint();
                        int count = input.read(buffer);
                        source.checkpoint();
                        if (count < 0) break;
                        if (count == 0) {
                            int value = input.read();
                            source.checkpoint();
                            if (value < 0) break;
                            buffer[0] = (byte)value;
                            count = 1;
                        }
                        if (count > limits.fileBytes - fileBytes || count > limits.bytes - bytes)
                            throw new IOException("Folder exceeds the skin size limit.");
                        stream.write(buffer, 0, count);
                        fileBytes += count;
                        bytes += count;
                    }
                }
            }
        }
    }
    private static void requireName(String name) throws IOException {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") ||
                name.indexOf('/') >= 0 || name.indexOf('\\') >= 0)
            throw new IOException("Folder contains an unsafe file name.");
        for (int i = 0; i < name.length(); i++) {
            if (Character.isISOControl(name.charAt(i))) throw new IOException("Folder contains an unsafe file name.");
        }
    }
    static void removeTree(Path path) throws IOException {
        if (!Files.exists(path, LinkOption.NOFOLLOW_LINKS)) return;
        // walkFileTree does not follow links, including a replaced final leaf.
        Files.walkFileTree(path, new SimpleFileVisitor<Path>() {
            @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) throws IOException {
                Files.delete(file); return FileVisitResult.CONTINUE;
            }
            @Override public FileVisitResult postVisitDirectory(Path directory, IOException error) throws IOException {
                if (error != null) throw error;
                Files.delete(directory); return FileVisitResult.CONTINUE;
            }
        });
    }
}
