package com.snurhythm.asobmashow;

import java.nio.file.Path;
import java.util.UUID;

/** Exact path grammar for private Android document imports. */
final class DocumentHandoffImportPathPolicy {
    private static final String IMPORT_FILE_NAME = "imported-document.zip";

    static boolean isIssuedPath(Path base, Path candidate) {
        return isIssuedPath(base, candidate, IMPORT_FILE_NAME);
    }

    static boolean isIssuedDirectoryPath(Path base, Path candidate) {
        return isIssuedPath(base, candidate, "imported-directory");
    }

    private static boolean isIssuedPath(Path base, Path candidate, String leaf) {
        if (base == null || candidate == null || candidate.getParent() == null ||
                !leaf.equals(candidate.getFileName().toString())) {
            return false;
        }
        Path issuedDirectory = candidate.getParent();
        if (!base.equals(issuedDirectory.getParent())) {
            return false;
        }
        String directoryName = issuedDirectory.getFileName().toString();
        try {
            return UUID.fromString(directoryName).toString().equals(directoryName);
        } catch (IllegalArgumentException ignored) {
            return false;
        }
    }

    private DocumentHandoffImportPathPolicy() {
    }
}
