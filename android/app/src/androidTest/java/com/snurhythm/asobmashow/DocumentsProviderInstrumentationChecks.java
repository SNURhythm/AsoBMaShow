package com.snurhythm.asobmashow;

import android.app.Instrumentation;
import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicReference;

final class DocumentsProviderInstrumentationChecks {
    private static final String ROOT_ID = "documents";
    private static final String ROOT_DOCUMENT_ID = "documents:";

    static void run(Context context, Instrumentation instrumentation) throws Exception {
        ContentResolver resolver = context.getContentResolver();
        String authority = BuildConfig.APPLICATION_ID + ".documents";
        Uri root = DocumentsContract.buildDocumentUri(authority, ROOT_DOCUMENT_ID);
        File base = context.getExternalFilesDir(null);
        if (base == null) base = context.getFilesDir();
        File nativeRoot = new File(base, "Documents");

        try (Cursor roots = resolver.query(DocumentsContract.buildRootsUri(authority),
                null, null, null, null)) {
            require(roots != null && roots.moveToFirst(), "Provider returned no root");
            require(ROOT_ID.equals(value(roots, Root.COLUMN_ROOT_ID)), "Wrong root ID");
            require(ROOT_DOCUMENT_ID.equals(value(roots, Root.COLUMN_DOCUMENT_ID)),
                    "Root does not identify the documents directory");
            int flags = roots.getInt(roots.getColumnIndexOrThrow(Root.COLUMN_FLAGS));
            require((flags & Root.FLAG_SUPPORTS_CREATE) != 0, "Root cannot create documents");
            require(!roots.moveToNext(), "Provider unexpectedly exposes multiple roots");
        }
        verifyDocument(resolver, root, Document.MIME_TYPE_DIR,
                Document.FLAG_DIR_SUPPORTS_CREATE,
                Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME);
        // No game Activity is launched and the test must not create Documents itself.
        require(nativeRoot.isDirectory(), "Provider did not initialize Documents before game launch");
        Map<String, String> rootChildren = children(resolver, authority, ROOT_DOCUMENT_ID);
        File[] nativeChildren = nativeRoot.listFiles();
        require(nativeChildren != null, "Cannot enumerate native documents directory");
        for (File child : nativeChildren) {
            if (!child.isHidden()) {
                require(rootChildren.containsKey(child.getName()),
                        "Provider omitted native root entry: " + child.getName());
            }
        }

        try (Cursor projected = resolver.query(root,
                new String[]{Document.COLUMN_DOCUMENT_ID}, null, null, null)) {
            require(projected != null && projected.moveToFirst() && projected.getColumnCount() == 1,
                    "Provider did not honor a narrow projection");
        }

        verifyPrivateBoundary(context, resolver, authority, nativeRoot);
        verifySelfImportRejected(authority, instrumentation);
        verifyWriterBatch(context, resolver, authority, nativeRoot);
        String fixtureName = "DocumentsProviderTest-" + UUID.randomUUID();
        File nativeFixture = new File(nativeRoot, fixtureName);
        Uri fixture = null;
        File outside = File.createTempFile("documents-provider-outside-", ".bin", context.getCacheDir());
        try {
            fixture = create(resolver, root, Document.MIME_TYPE_DIR, fixtureName);
            require(nativeFixture.isDirectory(), "Provider created a copy outside the native root");
            verifyDocument(resolver, fixture, Document.MIME_TYPE_DIR,
                    Document.FLAG_DIR_SUPPORTS_CREATE | Document.FLAG_SUPPORTS_DELETE
                            | Document.FLAG_SUPPORTS_RENAME, 0);
            require(DocumentsContract.isChildDocument(resolver, root, fixture),
                    "Root does not recognize its own child");
            require(!DocumentsContract.isChildDocument(resolver, fixture, root),
                    "A child incorrectly owns the root");

            // Both database and skin directories must be visible, with no chart-only filtering.
            Uri databases = create(resolver, fixture, Document.MIME_TYPE_DIR, "DBs");
            Uri skins = create(resolver, fixture, Document.MIME_TYPE_DIR, "Skins");
            Uri database = create(resolver, databases, "application/octet-stream", "library.db");
            Uri skin = create(resolver, skins, "application/json", "skin.json");
            require(new File(nativeFixture, "DBs/library.db").isFile(), "Database path differs from native storage");
            require(new File(nativeFixture, "Skins/skin.json").isFile(), "Skin path differs from native storage");
            Map<String, String> entries = children(resolver, authority, DocumentsContract.getDocumentId(fixture));
            require(entries.keySet().containsAll(Arrays.asList("DBs", "Skins")),
                    "Provider filtered database or skin directories");
            require(children(resolver, authority, DocumentsContract.getDocumentId(databases))
                    .containsKey("library.db"), "Provider filtered a database file");
            require(children(resolver, authority, DocumentsContract.getDocumentId(skins))
                    .containsKey("skin.json"), "Provider filtered a skin file");

            Uri file = create(resolver, fixture, "application/octet-stream", "payload.bin");
            File nativeFile = new File(nativeFixture, "payload.bin");
            verifyDocument(resolver, file, "application/octet-stream",
                    Document.FLAG_SUPPORTS_WRITE | Document.FLAG_SUPPORTS_DELETE
                            | Document.FLAG_SUPPORTS_RENAME, 0);
            byte[] bytes = new byte[96 * 1024 + 13];
            for (int index = 0; index < bytes.length; ++index) bytes[index] = (byte) (index * 31);
            write(resolver, file, bytes);
            require(Arrays.equals(bytes, read(resolver.openInputStream(file))), "Stream reopen changed file bytes");
            require(Arrays.equals(bytes, read(new FileInputStream(nativeFile))),
                    "Provider and native file have different contents");
            byte[] shorter = {11, 22, 33};
            write(resolver, file, shorter);
            require(nativeFile.length() == shorter.length, "Write/truncate left stale bytes behind");
            require(Arrays.equals(shorter, read(resolver.openInputStream(file))),
                    "Truncated stream did not reopen with the new contents");
            byte[] nativeBytes = {44, 55, 66, 77};
            try (OutputStream output = new FileOutputStream(nativeFile)) { output.write(nativeBytes); }
            require(Arrays.equals(nativeBytes, read(resolver.openInputStream(file))),
                    "Provider did not observe a native file edit");

            Uri renamed = DocumentsContract.renameDocument(resolver, file, "renamed.bin");
            require(renamed != null, "File rename returned no document");
            require(!nativeFile.exists() && new File(nativeFixture, "renamed.bin").isFile(),
                    "Provider file rename did not update the native path");
            require(Arrays.equals(nativeBytes, read(resolver.openInputStream(renamed))),
                    "File rename changed the contents");
            require(DocumentsContract.isChildDocument(resolver, fixture, database),
                    "Recursive descendants are not recognized");
            require(!DocumentsContract.isChildDocument(resolver, databases, skin),
                    "Sibling directory was treated as an ancestor");
            require(DocumentsContract.deleteDocument(resolver, renamed), "File delete failed");
            require(!new File(nativeFixture, "renamed.bin").exists(), "Deleted file remains on native storage");

            Uri renamedSkins = DocumentsContract.renameDocument(resolver, skins, "Skins Renamed");
            require(renamedSkins != null && !new File(nativeFixture, "Skins").exists(),
                    "Directory rename did not remove the old path");
            require(new File(nativeFixture, "Skins Renamed/skin.json").isFile(),
                    "Directory rename lost its child");
            require(children(resolver, authority, DocumentsContract.getDocumentId(renamedSkins))
                    .containsKey("skin.json"), "Renamed directory children cannot be queried");
            require(DocumentsContract.deleteDocument(resolver, renamedSkins), "Directory delete failed");
            require(!new File(nativeFixture, "Skins Renamed").exists(), "Directory delete left native children");

            // An existing sentinel outside the exported root distinguishes rejection from a missing file.
            try (OutputStream output = new FileOutputStream(outside)) { output.write(nativeBytes); }
            String outsidePath = outside.getCanonicalPath();
            String traversal = ROOT_DOCUMENT_ID + "../../../../../../../../" + outsidePath.substring(1);
            for (String invalidId : new String[] {"outside:" + outsidePath,
                    ROOT_DOCUMENT_ID + outsidePath, traversal,
                    ROOT_DOCUMENT_ID + fixtureName + "/../" + fixtureName + "/DBs/library.db"}) {
                Uri invalid = DocumentsContract.buildDocumentUri(authority, invalidId);
                requireRejected(() -> {
                    try (InputStream input = resolver.openInputStream(invalid)) {
                        if (input == null) throw new Rejected();
                        throw new AssertionError("Provider opened invalid document ID: " + invalidId);
                    }
                }, "Invalid document ID was not rejected: " + invalidId);
            }
            require(Arrays.equals(nativeBytes, read(new FileInputStream(outside))),
                    "Outside sentinel was changed");
            requireRejected(() -> {
                if (!DocumentsContract.deleteDocument(resolver, root)) throw new Rejected();
            }, "Provider allowed deleting its root");
            requireRejected(() -> {
                if (DocumentsContract.renameDocument(resolver, root, fixtureName + "-root") == null)
                    throw new Rejected();
            }, "Provider allowed renaming its root");
            require(nativeRoot.isDirectory() && nativeFixture.isDirectory(), "Root protection changed native storage");
        } finally {
            try {
                if (fixture != null) {
                    require(DocumentsContract.deleteDocument(resolver, fixture), "Could not clean owned provider fixture");
                    require(!nativeFixture.exists(), "Owned fixture remains after provider cleanup");
                }
            } finally {
                require(outside.delete(), "Could not clean owned outside-root sentinel");
            }
        }
    }

    private static void verifyPrivateBoundary(Context context, ContentResolver resolver,
                                              String authority, File nativeRoot) throws Exception {
        List<File> sentinels = new ArrayList<>();
        byte[] privateBytes = {91, 82, 73, 64};
        try {
            sentinels.add(File.createTempFile("documents-sibling-private-", ".bin", nativeRoot.getParentFile()));
            sentinels.add(File.createTempFile("documents-internal-private-", ".bin", context.getFilesDir()));
            sentinels.add(File.createTempFile("documents-cache-private-", ".bin", context.getCacheDir()));
            for (File sentinel : sentinels) {
                try (OutputStream output = new FileOutputStream(sentinel)) { output.write(privateBytes); }
            }
            Map<String, String> listed = children(resolver, authority, ROOT_DOCUMENT_ID);
            for (Map.Entry<String, String> entry : listed.entrySet()) {
                require(new File(nativeRoot, entry.getKey()).exists(),
                        "Provider enumerated an entry outside Documents: " + entry.getKey());
            }
            for (File sentinel : sentinels) {
                require(!listed.containsKey(sentinel.getName()), "Provider enumerated private sibling/cache data");
                String absolutePath = sentinel.getCanonicalPath();
                for (String id : new String[]{ROOT_DOCUMENT_ID + sentinel.getName(),
                        ROOT_DOCUMENT_ID + "../" + sentinel.getName(),
                        ROOT_DOCUMENT_ID + absolutePath,
                        ROOT_DOCUMENT_ID + "../../../../../../../../" + absolutePath.substring(1)}) {
                    Uri outside = DocumentsContract.buildDocumentUri(authority, id);
                    requireRejected(() -> {
                        try (InputStream input = resolver.openInputStream(outside)) {
                            if (input == null) throw new Rejected();
                            throw new AssertionError("Provider exposed private sibling/cache data: " + id);
                        }
                    }, "Private sibling/cache document was not rejected: " + id);
                }
                require(Arrays.equals(privateBytes, read(new FileInputStream(sentinel))),
                        "Provider boundary checks changed private data");
            }
        } finally {
            boolean cleaned = true;
            for (File sentinel : sentinels) cleaned &= sentinel.delete();
            require(cleaned, "Could not clean owned private-boundary sentinels");
        }
    }

    private static void verifySelfImportRejected(String authority, Instrumentation instrumentation) throws Exception {
        Method copy = AsoBMaShowActivity.class.getDeclaredMethod(
                "copyTreeUriToBmsFolder", Uri.class, String.class, ChartImportCopyControl.class);
        copy.setAccessible(true);
        AtomicReference<AsoBMaShowActivity> activity = new AtomicReference<>();
        instrumentation.runOnMainSync(() -> activity.set(new AsoBMaShowActivity()));
        try {
            // Rejection must happen before any copy/checkpoint or Activity storage access.
            copy.invoke(activity.get(),
                    DocumentsContract.buildTreeDocumentUri(authority, ROOT_DOCUMENT_ID),
                    "AsoBMaShow", null);
            throw new AssertionError("Own Documents tree can be imported recursively into itself");
        } catch (InvocationTargetException error) {
            require(error.getCause() instanceof IOException &&
                    error.getCause().getMessage().contains("already in AsoBMaShow"),
                    "Own Documents tree did not fail before starting the copy");
        }
    }

    private static void verifyWriterBatch(Context context, ContentResolver resolver,
                                           String authority, File nativeRoot) throws Exception {
        File bms = new File(nativeRoot, "BMS");
        require(bms.isDirectory() || bms.mkdir(), "Could not prepare BMS directory");
        Uri parent = DocumentsContract.buildDocumentUri(authority, ROOT_DOCUMENT_ID + "BMS");
        Uri folder = create(resolver, parent, Document.MIME_TYPE_DIR, "DocumentsProviderTest-" + UUID.randomUUID());
        DocumentsLibraryChanges changes = AsoBMaShowDocumentsProvider.changes(context);
        try {
            Uri file = create(resolver, folder, "application/octet-stream", "copy.bin");
            try (ParcelFileDescriptor writer = resolver.openFileDescriptor(file, "wt")) {
                require(writer != null, "Provider did not open a writer");
                require(changes.readyRevision(SystemClock.elapsedRealtime() + 100000) == 0,
                        "A live writer allowed premature library refresh");
            }
            long deadline = SystemClock.elapsedRealtime() + 5000;
            while (changes.readyRevision(SystemClock.elapsedRealtime()) == 0 &&
                    SystemClock.elapsedRealtime() < deadline) SystemClock.sleep(25);
            require(changes.readyRevision(SystemClock.elapsedRealtime()) != 0,
                    "Writer close failed to schedule a settled library refresh");
            require(context.getSharedPreferences("documents-provider", Context.MODE_PRIVATE)
                    .getBoolean("library-dirty", false), "Pending refresh was not persisted");
        } finally {
            require(DocumentsContract.deleteDocument(resolver, folder), "Could not clean BMS write fixture");
        }
    }

    private static Uri create(ContentResolver resolver, Uri parent, String mime, String name) throws Exception {
        Uri created = DocumentsContract.createDocument(resolver, parent, mime, name);
        require(created != null, "Create returned no document for " + name);
        return created;
    }

    private static void write(ContentResolver resolver, Uri file, byte[] bytes) throws Exception {
        try (OutputStream output = resolver.openOutputStream(file, "wt")) {
            require(output != null, "Provider returned no write stream");
            for (int offset = 0; offset < bytes.length; offset += 4096)
                output.write(bytes, offset, Math.min(4096, bytes.length - offset));
        }
    }

    private static byte[] read(InputStream source) throws Exception {
        require(source != null, "Provider returned no read stream");
        try (InputStream input = source; ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[4096];
            int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            return output.toByteArray();
        }
    }

    private static Map<String, String> children(ContentResolver resolver, String authority, String id) {
        Map<String, String> result = new HashMap<>();
        try (Cursor cursor = resolver.query(DocumentsContract.buildChildDocumentsUri(authority, id),
                null, null, null, null)) {
            require(cursor != null, "Provider returned no child cursor");
            while (cursor.moveToNext()) result.put(value(cursor, Document.COLUMN_DISPLAY_NAME),
                    value(cursor, Document.COLUMN_DOCUMENT_ID));
        }
        return result;
    }

    private static void verifyDocument(ContentResolver resolver, Uri document, String mime,
                                       int requiredFlags, int forbiddenFlags) {
        try (Cursor cursor = resolver.query(document, null, null, null, null)) {
            require(cursor != null && cursor.moveToFirst(), "Provider returned no document metadata");
            require(mime.equals(value(cursor, Document.COLUMN_MIME_TYPE)), "Wrong document MIME type");
            int flags = cursor.getInt(cursor.getColumnIndexOrThrow(Document.COLUMN_FLAGS));
            require((flags & requiredFlags) == requiredFlags && (flags & forbiddenFlags) == 0,
                    "Wrong document capability flags for " + document);
            require(!cursor.moveToNext(), "Document query returned multiple rows");
        }
    }

    private static String value(Cursor cursor, String column) {
        return cursor.getString(cursor.getColumnIndexOrThrow(column));
    }

    private interface CheckedOperation { void run() throws Exception; }
    private static final class Rejected extends Exception {}

    private static void requireRejected(CheckedOperation operation, String message) throws Exception {
        try {
            operation.run();
        } catch (java.io.FileNotFoundException | SecurityException | IllegalArgumentException
                 | UnsupportedOperationException | Rejected expected) {
            return;
        }
        throw new AssertionError(message);
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
