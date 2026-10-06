package com.snurhythm.asobmashow;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;
import java.util.concurrent.atomic.AtomicBoolean;

public final class DocumentsProviderTests {
    interface Checked { void run() throws Exception; }
    static void require(boolean value) { if (!value) throw new AssertionError(); }
    static void denied(Checked action) throws Exception {
        try { action.run(); } catch (IOException expected) { return; }
        throw new AssertionError("Unsafe document accepted");
    }
    public static void main(String[] args) throws Exception {
        Path temporary = Files.createTempDirectory("documents-policy-").toRealPath();
        try {
            File root = Files.createDirectory(temporary.resolve("root")).toFile();
            DocumentsPathPolicy policy = new DocumentsPathPolicy(root);
            File skins = Files.createDirectory(root.toPath().resolve("Skins")).toFile();
            File database = Files.createFile(root.toPath().resolve("scores.db")).toFile();
            require(policy.resolve("documents:").equals(root));
            require(policy.resolve(policy.documentId(database)).equals(database));
            require(policy.resolve("documents:Skins").equals(skins));
            require(policy.isChild("documents:", "documents:Skins"));
            require(!policy.isChild("documents:Skins", "documents:Skins"));
            denied(() -> policy.resolve("documents:../outside"));
            denied(() -> policy.resolve("documents:/etc/passwd"));
            denied(() -> policy.resolve("documents:Skins/../scores.db"));
            denied(() -> policy.resolve("other:Skins"));
            denied(() -> policy.resolve("documents:missing"));
            for (String name : new String[]{"", ".", "..", "../escape", "a/b", "a\\b", "a\u0000b"}) {
                denied(() -> policy.child(root, name));
            }
            File sibling = Files.createDirectory(temporary.resolve("root-other")).toFile();
            Files.createFile(sibling.toPath().resolve("secret"));
            denied(() -> policy.documentId(sibling));
            Files.createSymbolicLink(root.toPath().resolve("link"), sibling.toPath());
            denied(() -> new DocumentsPathPolicy(new File(root, "link")));
            denied(() -> policy.resolve("documents:link/secret"));
            denied(() -> policy.child(new File(root, "link"), "created"));
            require(policy.child(skins, "한글.lr2skin").getParentFile().equals(skins));
            require(DocumentsPathPolicy.affectsBms("documents:BMS"));
            require(DocumentsPathPolicy.affectsBms("documents:BMS/song/a.wav"));
            require(!DocumentsPathPolicy.affectsBms("documents:Skins/a"));
            require(!DocumentsPathPolicy.affectsBms("documents:BMS-other"));

            AtomicBoolean persisted = new AtomicBoolean();
            DocumentsLibraryChanges changes = new DocumentsLibraryChanges(false, persisted::set);
            require(changes.readyRevision(5000) == 0);
            for (int i = 0; i < 1000; ++i) changes.writerOpened(i);
            require(persisted.get());
            require(changes.readyRevision(10000) == 0);
            for (int i = 0; i < 999; ++i) changes.writerClosed(1000 + i);
            require(changes.readyRevision(10000) == 0);
            changes.writerClosed(2000);
            require(changes.readyRevision(3999) == 0);
            long revision = changes.readyRevision(4000);
            require(revision != 0);
            changes.changed(4001);
            changes.acknowledge(revision);
            require(persisted.get());
            changes.acknowledge(changes.readyRevision(6001));
            require(!persisted.get());
            require(changes.readyRevision(9000) == 0);
            changes.changed(9001);
            DocumentsLibraryChanges restarted = new DocumentsLibraryChanges(persisted.get(), persisted::set);
            require(restarted.readyRevision(5000) != 0);
            restarted.acknowledge(restarted.readyRevision(5000));
            require(!persisted.get());
            System.out.println("PASS Documents path boundaries and 1000-writer refresh coalescing");
        } finally {
            try (var paths = Files.walk(temporary)) {
                for (Path path : paths.sorted(Comparator.reverseOrder()).toList()) Files.delete(path);
            }
        }
    }
}
