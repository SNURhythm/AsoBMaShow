package com.snurhythm.asobmashow;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.provider.DocumentsContract;
import java.nio.file.*;
import java.util.Arrays;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicReference;

final class SkinDirectoryInstrumentationChecks {
    static void run(Context context, Instrumentation instrumentation) throws Exception {
        verifyIncompleteListings();
        Path documents = AsoBMaShowDocumentsProvider.initializeDocuments(context).resolve(DocumentsPathPolicy.ROOT_DOCUMENT_ID).toPath();
        String name = "SkinDirectoryTest-" + UUID.randomUUID();
        Path fixture = Files.createDirectory(documents.resolve(name));
        Path sentinel = Files.createTempFile(context.getCacheDir().toPath(), "skin-outside-", ".bin");
        AtomicReference<AsoBMaShowActivity> instance = new AtomicReference<>();
        Uri tree = DocumentsContract.buildTreeDocumentUri(BuildConfig.APPLICATION_ID + ".documents", "documents:" + name);
        instrumentation.runOnMainSync(() -> {
            class PickerActivity extends AsoBMaShowActivity {
                PickerActivity() { attachBaseContext(context); }
                @Override public void startActivityForResult(Intent intent, int code) {
                    require(Intent.ACTION_OPEN_DOCUMENT_TREE.equals(intent.getAction()), "Not a tree picker");
                    require((intent.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0, "No read grant");
                    onActivityResult(code, Activity.RESULT_OK, new Intent().setData(tree));
                }
            }
            instance.set(new PickerActivity());
        });
        AsoBMaShowActivity activity = instance.get();
        // The fixture constructs an Activity without onCreate; progress now
        // crosses JNI even though this test does not launch the game loop.
        activity.loadLibraries();
        String path = null;
        try {
            Files.createDirectory(fixture.resolve("이미지"));
            Files.write(fixture.resolve("이미지/note.png"), new byte[]{1, 2, 3});
            Files.write(fixture.resolve("skin.lua"), new byte[]{4, 5, 6});
            String token = "skin-directory-" + UUID.randomUUID();
            require("__OK__".equals(activity.registerDocumentHandoff(token)), "Could not register handoff");
            String result;
            try { result = activity.importDirectory(token, "6,3,2,128,3"); }
            finally { activity.retireDocumentHandoff(token); }
            require(result.startsWith("/"), "Folder bridge failed: " + result);
            int separator = result.indexOf('\n');
            require(separator > 0 && result.substring(separator + 1).equals(name), "Original folder name lost");
            path = result.substring(0, separator);
            require("__OK__".equals(activity.validateDirectoryHandoffImport(path)), "Folder ownership rejected");
            require(activity.validateDocumentHandoffImport(path).startsWith("__ERROR__:"), "Folder authorized as archive");
            require(Arrays.equals(Files.readAllBytes(new java.io.File(path, "이미지/note.png").toPath()), new byte[]{1, 2, 3}), "Asset path or bytes changed");
            require(Files.exists(fixture.resolve("skin.lua")), "Source was modified");
            Files.createSymbolicLink(new java.io.File(path, "outside").toPath(), sentinel);
            require("__OK__".equals(activity.cleanupDirectoryHandoffImport(path)), "Folder cleanup failed");
            require(Files.exists(sentinel), "Cleanup followed a link");
            require(!Files.exists(new java.io.File(path).toPath()), "Temporary folder leaked");
            require("__OK__".equals(activity.cleanupDirectoryHandoffImport(path)), "Cleanup is not idempotent");
            require(activity.cleanupDirectoryHandoffImport(fixture.toString()).startsWith("__ERROR__:"), "Arbitrary folder authorized");
            path = null;
            Path archiveParent = Files.createDirectory(context.getCacheDir().getCanonicalFile().toPath()
                    .resolve("document-handoff").resolve(UUID.randomUUID().toString()));
            Path archive = Files.write(archiveParent.resolve("imported-document.zip"), new byte[]{7});
            require("__OK__".equals(activity.validateDocumentHandoffImport(archive.toString())), "Canonical archive ownership rejected");
            require(activity.validateDirectoryHandoffImport(archive.toString()).startsWith("__ERROR__:"), "Archive authorized as folder");
            require("__OK__".equals(activity.cleanupDocumentHandoffImport(archive.toString())), "Archive cleanup regressed");
            // Actual provider streams must enforce bounds and clean failed staging.
            Path base = context.getCacheDir().toPath().resolve("document-handoff");
            long before;
            try (java.util.stream.Stream<Path> entries = Files.list(base)) { before = entries.count(); }
            token = "skin-directory-" + UUID.randomUUID();
            activity.registerDocumentHandoff(token);
            try { result = activity.importDirectory(token, "5,3,2,128,3"); }
            finally { activity.retireDocumentHandoff(token); }
            require(result.startsWith("__ERROR__:"), "Oversized tree accepted");
            try (java.util.stream.Stream<Path> entries = Files.list(base)) { require(entries.count() == before, "Failed import leaked staging"); }
            token = "skin-directory-" + UUID.randomUUID();
            activity.registerDocumentHandoff(token);
            activity.cancelDocumentHandoff(token);
            try { require("__CANCELLED__".equals(activity.importDirectory(token, "6,3,2,128,3")), "Cancellation ignored"); }
            finally { activity.retireDocumentHandoff(token); }
            require(activity.getDocumentsMutationLock() == AsoBMaShowDocumentsProvider.DOCUMENT_MUTATION_LOCK,
                    "Native bridge and provider do not share the same mutation monitor");
            try (android.content.ContentProviderClient client = context.getContentResolver()
                    .acquireContentProviderClient(BuildConfig.APPLICATION_ID + ".documents")) {
                AsoBMaShowDocumentsProvider provider = (AsoBMaShowDocumentsProvider)client.getLocalContentProvider();
                String created = whileMutationBlocked(() -> provider.createDocument("documents:" + name,
                        "application/octet-stream", "locked-file"));
                whileMutationBlocked(() -> {
                    try (android.os.ParcelFileDescriptor opened = provider.openDocument(created, "w", null)) { }
                    return null;
                });
                String renamed = whileMutationBlocked(() -> provider.renameDocument(created, "renamed-file"));
                whileMutationBlocked(() -> { provider.deleteDocument(renamed); return null; });
            }
        } finally {
            if (path != null) activity.cleanupDirectoryHandoffImport(path);
            SkinDirectoryImport.removeTree(fixture);
            Files.deleteIfExists(sentinel);
        }
    }
    private static <T> T whileMutationBlocked(java.util.concurrent.Callable<T> action) throws Exception {
        java.util.concurrent.FutureTask<T> task = new java.util.concurrent.FutureTask<>(action);
        Thread worker = new Thread(task, "document-mutation-check");
        synchronized (AsoBMaShowDocumentsProvider.DOCUMENT_MUTATION_LOCK) {
            worker.start();
            long deadline = android.os.SystemClock.elapsedRealtime() + 5000;
            while (worker.getState() != Thread.State.BLOCKED && !task.isDone()
                    && android.os.SystemClock.elapsedRealtime() < deadline) Thread.yield();
            require(worker.getState() == Thread.State.BLOCKED && !task.isDone(),
                    "Provider mutation bypassed native publication monitor");
        }
        return task.get(5, java.util.concurrent.TimeUnit.SECONDS);
    }

    private static void verifyIncompleteListings() throws Exception {
        if (android.os.Build.VERSION.SDK_INT < 29) return;
        for (boolean loading : new boolean[]{false, true}) {
            for (boolean late : new boolean[]{false, true}) {
                android.content.ContentProvider provider = new android.content.ContentProvider() {
                    @Override public boolean onCreate() { return true; }
                    @Override public String getType(Uri uri) { return DocumentsContract.Document.MIME_TYPE_DIR; }
                    @Override public Uri insert(Uri uri, android.content.ContentValues values) { return null; }
                    @Override public int update(Uri uri, android.content.ContentValues values,
                                                String selection, String[] args) { return 0; }
                    @Override public int delete(Uri uri, String selection, String[] args) {
                        throw new AssertionError("Import must not delete source documents");
                    }
                    @Override public android.database.Cursor query(Uri uri, String[] projection, String selection,
                                                                    String[] args, String order) {
                        android.database.MatrixCursor cursor = new android.database.MatrixCursor(projection) {
                            int reads;
                            @Override public android.os.Bundle getExtras() {
                                android.os.Bundle extras = new android.os.Bundle();
                                if (!late || reads++ > 0) {
                                    if (loading) extras.putBoolean(DocumentsContract.EXTRA_LOADING, true);
                                    else extras.putString(DocumentsContract.EXTRA_ERROR, "Provider unavailable");
                                }
                                return extras;
                            }
                        };
                        cursor.addRow(new Object[]{"root", "root", DocumentsContract.Document.MIME_TYPE_DIR, 0L});
                        return cursor;
                    }
                };
                SafSkinDirectorySource source = new SafSkinDirectorySource(android.content.ContentResolver.wrap(provider),
                        DocumentsContract.buildTreeDocumentUri("test", "root"), new android.os.CancellationSignal(),
                        new SafSkinDirectorySource.IoControl() {
                            public void checkpoint() { }
                            public void descriptor(android.os.ParcelFileDescriptor descriptor) { }
                        });
                try { source.root(); throw new AssertionError("Partial root accepted"); }
                catch (java.io.IOException expected) { }
                try { source.children(new SkinDirectoryImport.Entry("root", "root", true, 0), 10);
                    throw new AssertionError("Partial children accepted"); }
                catch (java.io.IOException expected) { }
            }
        }
    }
    private static void require(boolean condition, String message) { if (!condition) throw new AssertionError(message); }
}
