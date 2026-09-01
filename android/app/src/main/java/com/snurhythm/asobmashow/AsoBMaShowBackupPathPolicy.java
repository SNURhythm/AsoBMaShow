package com.snurhythm.asobmashow;

import java.io.IOException;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;

final class AsoBMaShowBackupPathPolicy {
    private static final String CREDENTIAL_FILE = "ir-credentials.json";

    interface EntryConsumer {
        void accept(Path path) throws IOException;
    }

    private AsoBMaShowBackupPathPolicy() {}

    static void visitBackupEntries(
            Path root,
            Collection<Path> excludedRoots,
            Collection<Path> credentialProfileRoots,
            EntryConsumer consumer) throws IOException {
        Path normalizedRoot = normalize(root);
        if (!Files.exists(normalizedRoot)) {
            return;
        }
        List<Path> normalizedExcludedRoots = normalize(excludedRoots);
        List<Path> normalizedProfileRoots = normalize(credentialProfileRoots);

        Files.walkFileTree(normalizedRoot, new SimpleFileVisitor<Path>() {
            @Override
            public FileVisitResult preVisitDirectory(
                    Path directory, BasicFileAttributes attributes) throws IOException {
                Path normalized = normalize(directory);
                if (isInsideAny(normalized, normalizedExcludedRoots)
                        || isCredentialArtifact(normalized, normalizedProfileRoots)) {
                    return FileVisitResult.SKIP_SUBTREE;
                }
                consumer.accept(normalized);
                return FileVisitResult.CONTINUE;
            }

            @Override
            public FileVisitResult visitFile(
                    Path file, BasicFileAttributes attributes) throws IOException {
                Path normalized = normalize(file);
                if (!attributes.isSymbolicLink()
                        && attributes.isRegularFile()
                        && !isInsideAny(normalized, normalizedExcludedRoots)
                        && !isCredentialArtifact(normalized, normalizedProfileRoots)) {
                    consumer.accept(normalized);
                }
                return FileVisitResult.CONTINUE;
            }
        });
    }

    static boolean isCredentialArtifact(
            Path candidate, Collection<Path> credentialProfileRoots) {
        Path normalizedCandidate = normalize(candidate);
        for (Path profileRoot : credentialProfileRoots) {
            Path normalizedProfileRoot = normalize(profileRoot);
            if (!normalizedCandidate.startsWith(normalizedProfileRoot)) {
                continue;
            }
            Path relative = normalizedProfileRoot.relativize(normalizedCandidate);
            if (relative.getNameCount() != 2) {
                continue;
            }
            String filename = relative.getName(1).toString();
            if (filename.equals(CREDENTIAL_FILE)
                    || filename.startsWith(CREDENTIAL_FILE + ".")) {
                return true;
            }
        }
        return false;
    }

    private static boolean isInsideAny(Path candidate, Collection<Path> roots) {
        for (Path root : roots) {
            if (candidate.startsWith(root)) {
                return true;
            }
        }
        return false;
    }

    private static List<Path> normalize(Collection<Path> paths) {
        List<Path> normalized = new ArrayList<>(paths.size());
        for (Path path : paths) {
            if (path != null) {
                normalized.add(normalize(path));
            }
        }
        return normalized;
    }

    private static Path normalize(Path path) {
        return path.toAbsolutePath().normalize();
    }
}
