package com.snurhythm.asobmashow;

import android.content.Context;
import android.database.MatrixCursor;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract.Document;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Comparator;

public final class DocumentsMutabilityTests {
    private static final String ROOT = "documents:";
    private static final byte[] SENTINEL = {1, 7, 23, 91};
    private static final int MUTATIONS = Document.FLAG_SUPPORTS_WRITE | Document.FLAG_SUPPORTS_DELETE
            | Document.FLAG_SUPPORTS_RENAME | Document.FLAG_DIR_SUPPORTS_CREATE;
    interface Action { void run() throws Exception; }

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    private static void denied(Action action) throws Exception {
        try { action.run(); }
        catch (IOException | SecurityException expected) { return; }
        throw new AssertionError("Protected mutation was accepted");
    }

    private static void seed(Path root, String relative) throws IOException {
        Path file = root.resolve(relative);
        Files.createDirectories(file.getParent());
        Files.write(file, SENTINEL);
    }

    private static int flags(AsoBMaShowDocumentsProvider provider, String relative) throws IOException {
        MatrixCursor cursor = (MatrixCursor)provider.queryDocument(ROOT + relative, null);
        check(cursor.rows.size() == 1, "Expected one document row");
        return (Integer)cursor.rows.get(0).get(Document.COLUMN_FLAGS);
    }

    private static void importReservation(AsoBMaShowDocumentsProvider provider, Path documents, boolean move)
            throws Exception {
        String bms = provider.createDocument(ROOT, Document.MIME_TYPE_DIR, "BMS");
        Path output = documents.resolve("BMS/Imported");
        check(AsoBMaShowDocumentsProvider.DOCUMENT_MUTATION_LOCK == DocumentsMutationGuard.LOCK,
                "Provider and import must share the same mutation monitor");
        try (DocumentsMutationGuard.Reservation reservation = DocumentsMutationGuard.reserveNewDestination(output.toFile())) {
            denied(() -> provider.createDocument(bms, Document.MIME_TYPE_DIR, "Imported"));
            denied(() -> provider.deleteDocument(bms));
            denied(() -> provider.renameDocument(bms, "MovedBMS"));
            String sibling = provider.createDocument(bms, Document.MIME_TYPE_DIR, "Sibling");
            provider.deleteDocument(sibling);
            if (!move) {
                seed(documents, "BMS/Imported/chart.bms");
            } else {
                ChartFolderImport.Entry root = new ChartFolderImport.Entry("root", "root", true, -1, 100);
                ChartFolderImport.Entry album = new ChartFolderImport.Entry("album", "Album", true, -1, 100);
                ChartFolderImport.Entry chart = new ChartFolderImport.Entry("chart", "chart.bms", false, SENTINEL.length, 100);
                ChartFolderImport.Entry nestedChart = new ChartFolderImport.Entry("nested", "chart.bms", false, SENTINEL.length, 100);
                java.util.Set<String> removed = new java.util.HashSet<>();
                ChartFolderImport.Source source = new ChartFolderImport.Source() {
                    public ChartFolderImport.Entry root() { return root; }
                    public java.util.List<ChartFolderImport.Entry> children(ChartFolderImport.Entry directory) {
                        if (directory == root) return removed.contains("album") ? java.util.List.of(chart)
                                : java.util.List.of(album, chart);
                        return java.util.List.of(nestedChart);
                    }
                    public java.io.InputStream open(ChartFolderImport.Entry file) {
                        return new java.io.ByteArrayInputStream(SENTINEL);
                    }
                    public void delete(ChartFolderImport.Entry directory) throws IOException {
                        check(Thread.holdsLock(DocumentsMutationGuard.LOCK),
                                "Source removal must be serialized with Files mutations");
                        try {
                            verifyImportBlocked(provider, "BMS/Imported/Album/chart.bms");
                            if (directory == root) verifyImportBlocked(provider, "BMS/Imported/chart.bms");
                        } catch (Exception error) { throw new IOException(error); }
                        removed.add(directory.id);
                    }
                };
                ChartFolderImport.Result result = ChartFolderImport.run(source, reservation.canonicalOutput,
                        true, new ChartImportCopyControl(() -> 1, () -> false),
                        (copied, total, bytes, totalBytes, name, phase) -> {}, () -> {}, DocumentsMutationGuard.LOCK);
                check(result.complete && removed.contains("root") && removed.contains("album"),
                        "Reserved move did not finish: " + result.error);
                check(Arrays.equals(Files.readAllBytes(output.resolve("Album/chart.bms")), SENTINEL),
                        "Previously moved child copy was damaged");
            }
            verifyImportBlocked(provider, "BMS/Imported/chart.bms");
            try (ParcelFileDescriptor descriptor = provider.openDocument(ROOT + "BMS/Imported/chart.bms", "r", null)) {
                check(Arrays.equals(descriptor.readBytes(), SENTINEL), "Import reservation must allow reads");
            }
        }
        // Releasing the lease restores every normal Files operation.
        String created = provider.createDocument(ROOT + "BMS/Imported", "application/octet-stream", "new.bms");
        try (ParcelFileDescriptor descriptor = provider.openDocument(created, "rwt", null)) {
            descriptor.writeBytes(SENTINEL);
        }
        String renamed = provider.renameDocument(ROOT + "BMS/Imported", "Finished");
        provider.deleteDocument(renamed);
        check(!Files.exists(documents.resolve("BMS/Finished")), "Released reservation still blocks Files");
    }

    private static void verifyImportBlocked(AsoBMaShowDocumentsProvider provider, String relative) throws Exception {
        for (String mode : new String[]{"w", "wt", "wa", "rw", "rwt"}) {
            denied(() -> provider.openDocument(ROOT + relative, mode, null));
        }
        denied(() -> provider.deleteDocument(ROOT + relative));
        denied(() -> provider.renameDocument(ROOT + relative, "changed.bms"));
        denied(() -> provider.createDocument(ROOT + "BMS/Imported", "application/octet-stream", "other.bms"));
        denied(() -> provider.deleteDocument(ROOT + "BMS/Imported"));
        denied(() -> provider.renameDocument(ROOT + "BMS/Imported", "Moved"));
        denied(() -> provider.deleteDocument(ROOT + "BMS"));
        denied(() -> provider.renameDocument(ROOT + "BMS", "MovedBMS"));
    }

    public static void main(String[] args) throws Exception {
        Path temporary = Files.createTempDirectory("documents-mutability-").toRealPath();
        try {
            Context context = new Context(temporary.toFile());
            AsoBMaShowDocumentsProvider provider = new AsoBMaShowDocumentsProvider();
            provider.attachContext(context);
            provider.queryRoots(null);
            AsoBMaShowDocumentsProvider.initializeDocuments(context);
            Path documents = temporary.resolve("Documents");
            String scenario = args[0];
            String[] databases = {"db/chart.db", "db/chart.db-wal", "db/chart.db-shm",
                    "profiles/player/scores.db", "profiles/player/replays.db-journal"};
            if (scenario.equals("reserved-import") || scenario.equals("reserved-move")) {
                importReservation(provider, documents, scenario.equals("reserved-move"));
            } else if (scenario.startsWith("write-")) {
                for (String relative : databases) {
                    seed(documents, relative);
                    int opened = ParcelFileDescriptor.opened;
                    denied(() -> {
                        try (ParcelFileDescriptor descriptor = provider.openDocument(
                                ROOT + relative, scenario.substring(6), null)) {
                            descriptor.writeBytes(new byte[]{99});
                        }
                    });
                    check(ParcelFileDescriptor.opened == opened, "Write rejection occurred after opening the file");
                    check(Arrays.equals(Files.readAllBytes(documents.resolve(relative)), SENTINEL),
                            "Rejected write changed database contents");
                }
            } else if (scenario.equals("read-and-flags")) {
                for (String relative : databases) {
                    seed(documents, relative);
                    check((flags(provider, relative) & MUTATIONS) == 0, "Database advertises mutation support");
                    try (ParcelFileDescriptor descriptor = provider.openDocument(ROOT + relative, "r", null)) {
                        check(Arrays.equals(descriptor.readBytes(), SENTINEL), "Protected database is not readable");
                    }
                }
                for (String relative : new String[]{"db", "profiles", "profiles/player"}) {
                    check((flags(provider, relative) & MUTATIONS) == 0, "Protected directory advertises mutations");
                    MatrixCursor children = (MatrixCursor)provider.queryChildDocuments(ROOT + relative, null, null);
                    check(!children.rows.isEmpty(), "Protected subtree must remain visible");
                }
                check((flags(provider, "") & Document.FLAG_DIR_SUPPORTS_CREATE) != 0,
                        "Documents root must still support normal creation");
            } else if (scenario.equals("ancestors")) {
                for (String relative : databases) seed(documents, relative);
                for (String relative : new String[]{"", "db", "db/chart.db", "profiles", "profiles/player",
                        "profiles/player/scores.db"}) {
                    denied(() -> provider.renameDocument(ROOT + relative, "moved"));
                    denied(() -> provider.deleteDocument(ROOT + relative));
                }
                for (String relative : databases) {
                    check(Arrays.equals(Files.readAllBytes(documents.resolve(relative)), SENTINEL),
                            "Ancestor mutation damaged protected descendants");
                }
            } else if (scenario.equals("create-reserved")) {
                for (String name : new String[]{"db", "DB", "Db", "profiles", "PROFILES"}) {
                    for (String mime : new String[]{Document.MIME_TYPE_DIR, "application/octet-stream"}) {
                        denied(() -> provider.createDocument(ROOT, mime, name));
                        check(!Files.exists(documents.resolve(name)), "Reserved creation changed storage");
                    }
                }
            } else if (scenario.equals("rename-reserved")) {
                String source = provider.createDocument(ROOT, Document.MIME_TYPE_DIR, "ordinary");
                for (String name : new String[]{"db", "DB", "profiles", "PROFILES"}) {
                    denied(() -> provider.renameDocument(source, name));
                    check(Files.isDirectory(documents.resolve("ordinary")), "Reserved rename moved source");
                }
            } else if (scenario.equals("create-inside")) {
                for (String relative : databases) seed(documents, relative);
                for (String parent : new String[]{"db", "profiles", "profiles/player"}) {
                    denied(() -> provider.createDocument(ROOT + parent, "application/octet-stream", "new.db"));
                    denied(() -> provider.createDocument(ROOT + parent, Document.MIME_TYPE_DIR, "new-folder"));
                }
                denied(() -> provider.createDocument(ROOT, Document.MIME_TYPE_DIR, "db"));
                check(!Files.exists(documents.resolve("db (1)")), "Reserved name was silently collision-suffixed");
            } else if (scenario.equals("ordinary")) {
                for (String folder : new String[]{"BMS", "Skins", "exports", "db-backup", "profiles-backup"}) {
                    String parent = folder.equals("Skins") ? ROOT + folder
                            : provider.createDocument(ROOT, Document.MIME_TYPE_DIR, folder);
                    String nested = provider.createDocument(parent, Document.MIME_TYPE_DIR, "db");
                    String file = provider.createDocument(nested, "application/octet-stream", "user.db");
                    check((flags(provider, file.substring(ROOT.length())) & Document.FLAG_SUPPORTS_WRITE) != 0,
                            "Ordinary database-like files must stay writable");
                    try (ParcelFileDescriptor descriptor = provider.openDocument(file, "rwt", null)) {
                        descriptor.writeBytes(SENTINEL);
                    }
                    String renamed = provider.renameDocument(file, "renamed.db");
                    try (ParcelFileDescriptor descriptor = provider.openDocument(renamed, "r", null)) {
                        check(Arrays.equals(descriptor.readBytes(), SENTINEL), "Ordinary rename changed bytes");
                    }
                    provider.deleteDocument(parent);
                    check(!Files.exists(documents.resolve(folder)), "Ordinary directory delete failed");
                }
            } else if (scenario.equals("aliases")) {
                seed(documents, "DB/chart.db");
                seed(documents, "PrOfIlEs/player/scores.db");
                for (String path : new String[]{"DB/chart.db", "PrOfIlEs/player/scores.db"}) {
                    denied(() -> provider.openDocument(ROOT + path, "rwt", null));
                }
                Files.createSymbolicLink(documents.resolve("alias"), documents.resolve("DB"));
                for (String path : new String[]{"alias/chart.db", "Skins/../DB/chart.db", "/DB/chart.db",
                        "DB//chart.db", "DB/./chart.db", "DB\\chart.db"}) {
                    denied(() -> provider.openDocument(ROOT + path, "rwt", null));
                }
                check(Arrays.equals(Files.readAllBytes(documents.resolve("DB/chart.db")), SENTINEL),
                        "Path alias bypass changed database contents");
            } else {
                throw new AssertionError("Unknown scenario: " + scenario);
            }
        } finally {
            try (var paths = Files.walk(temporary)) {
                for (Path path : paths.sorted(Comparator.reverseOrder()).toList()) Files.delete(path);
            }
        }
    }
}
