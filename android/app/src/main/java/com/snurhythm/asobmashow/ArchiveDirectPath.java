package com.snurhythm.asobmashow;

import java.io.File;
import java.io.IOException;

/** Resolves only a known local volume's document ID, never an arbitrary URI. */
final class ArchiveDirectPath {
    static File resolve(File root, String relative) {
        if (root == null || relative == null || relative.isEmpty() || relative.startsWith("/")) return null;
        for (String part : relative.split("/", -1)) {
            if (part.isEmpty() || part.equals(".") || part.equals("..")) return null;
        }
        try {
            File base = root.getCanonicalFile();
            File file = new File(base, relative).getCanonicalFile();
            return within(base, file) && file.isFile() && file.canRead() ? file : null;
        } catch (IOException | SecurityException error) { return null; }
    }

    static boolean within(File root, File file) {
        if (root == null || file == null) return false;
        try { return file.getCanonicalFile().toPath().startsWith(root.getCanonicalFile().toPath()); }
        catch (IOException | SecurityException error) { return false; }
    }
}
