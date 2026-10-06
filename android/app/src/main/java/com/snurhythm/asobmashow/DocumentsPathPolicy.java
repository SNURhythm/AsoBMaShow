package com.snurhythm.asobmashow;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

/** Stable, root-relative IDs; neither IDs nor display names may escape Documents. */
final class DocumentsPathPolicy {
    static final String ROOT_ID = "documents";
    static final String ROOT_DOCUMENT_ID = "documents:";
    private final File root;

    DocumentsPathPolicy(File root) throws IOException {
        if (Files.isSymbolicLink(root.toPath())) {
            throw new FileNotFoundException("Documents root cannot be a symbolic link");
        }
        this.root = root.getCanonicalFile();
    }

    File resolve(String documentId) throws IOException {
        if (documentId == null || !documentId.startsWith(ROOT_DOCUMENT_ID)) {
            throw new FileNotFoundException("Unknown document");
        }
        String relative = documentId.substring(ROOT_DOCUMENT_ID.length());
        File file = root;
        if (!relative.isEmpty()) {
            for (String part : relative.split("/", -1)) file = child(file, part);
        }
        if (!file.exists()) throw new FileNotFoundException("Document no longer exists");
        return file;
    }

    String documentId(File file) throws IOException {
        File checked = checked(file);
        return ROOT_DOCUMENT_ID + root.toPath().relativize(checked.toPath()).toString()
                .replace(File.separatorChar, '/');
    }

    File child(File parent, String name) throws IOException {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") ||
                name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 || name.indexOf('\0') >= 0) {
            throw new FileNotFoundException("Invalid document name");
        }
        return checked(new File(checked(parent), name));
    }

    boolean isReadOnly(File file) throws IOException {
        File canonical = checked(file);
        if (canonical.equals(root)) return false;
        String topLevel = root.toPath().relativize(canonical.toPath()).getName(0).toString();
        // These trees contain live SQLite databases and their WAL/SHM files.
        // Reserve the names even before initialization, including case aliases
        // on external storage. Reading and listing remain available for export.
        return topLevel.equalsIgnoreCase("db") || topLevel.equalsIgnoreCase("profiles");
    }

    void requireWritable(File file) throws IOException {
        if (isReadOnly(file)) {
            throw new FileNotFoundException("Application database and profile storage is read-only");
        }
    }

    private File checked(File file) throws IOException {
        Path path = file.getAbsoluteFile().toPath();
        Path rootPath = root.toPath();
        if (!path.startsWith(rootPath)) throw new FileNotFoundException("Outside Documents");
        Path current = rootPath;
        for (Path part : rootPath.relativize(path)) {
            current = current.resolve(part);
            if (Files.isSymbolicLink(current)) throw new FileNotFoundException("Symbolic links are not shared");
        }
        File canonical = file.getCanonicalFile();
        if (!canonical.toPath().startsWith(rootPath)) throw new FileNotFoundException("Outside Documents");
        return canonical;
    }

    boolean isChild(String parentId, String childId) throws IOException {
        File parent = resolve(parentId);
        File child = resolve(childId);
        return !parent.equals(child) && child.toPath().startsWith(parent.toPath());
    }

    static boolean affectsBms(String documentId) {
        return documentId.equals(ROOT_DOCUMENT_ID + "BMS") ||
                documentId.startsWith(ROOT_DOCUMENT_ID + "BMS/");
    }
}
