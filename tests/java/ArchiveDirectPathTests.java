package com.snurhythm.asobmashow;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;

public final class ArchiveDirectPathTests {
    static void require(boolean value) { if (!value) throw new AssertionError(); }
    public static void main(String[] args) throws Exception {
        Path temporary=Files.createTempDirectory("archive-direct-path-");
        try {
            File root=Files.createDirectory(temporary.resolve("volume")).toFile();
            File archive=Files.writeString(root.toPath().resolve("file.zip"), "archive").toFile();
            require(archive.getCanonicalFile().equals(ArchiveDirectPath.resolve(root, "file.zip")));
            require(ArchiveDirectPath.resolve(root, "../file.zip")==null);
            require(ArchiveDirectPath.resolve(root, "/file.zip")==null);
            require(ArchiveDirectPath.resolve(root, "./file.zip")==null);
            require(ArchiveDirectPath.resolve(root, "missing.zip")==null);
            require(ArchiveDirectPath.resolve(root, "")==null);
            Path outside=Files.writeString(temporary.resolve("outside.zip"), "outside");
            Files.createSymbolicLink(root.toPath().resolve("escape.zip"), outside);
            require(ArchiveDirectPath.resolve(root, "escape.zip")==null);
        } finally {
            try (var paths=Files.walk(temporary)) {
                paths.sorted(java.util.Comparator.reverseOrder()).forEach(path -> { try { Files.delete(path); } catch (Exception e) { throw new RuntimeException(e); } });
            }
        }
        System.out.println("Archive direct path tests passed");
    }
}
