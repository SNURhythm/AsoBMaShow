package com.snurhythm.asobmashow;

import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/** Publishes Documents without putting SAF on the game's native file-reading path. */
public final class AsoBMaShowDocumentsProvider extends DocumentsProvider {
    // Also held by native skin publication on Android FUSE without renameat2 flags.
    static final Object DOCUMENT_MUTATION_LOCK = DocumentsMutationGuard.LOCK;

    static final String AUTHORITY = BuildConfig.APPLICATION_ID + ".documents";
    private static final String[] ROOT_COLUMNS = {
            Root.COLUMN_ROOT_ID, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_TITLE,
            Root.COLUMN_FLAGS, Root.COLUMN_ICON, Root.COLUMN_AVAILABLE_BYTES, Root.COLUMN_MIME_TYPES
    };
    private static final String[] DOCUMENT_COLUMNS = {
            Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
            Document.COLUMN_FLAGS, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED
    };
    private static DocumentsLibraryChanges libraryChanges;
    private final Handler closeHandler = new Handler(Looper.getMainLooper());

    static File documentsDirectory(Context context) {
        // Match Utils::GetDocumentsPath; private app files stay outside this subtree.
        File external = context.getExternalFilesDir(null);
        return new File(external != null ? external : context.getFilesDir(), "Documents");
    }

    static synchronized DocumentsLibraryChanges changes(Context context) {
        if (libraryChanges == null) {
            SharedPreferences preferences = context.getApplicationContext().getSharedPreferences(
                    "documents-provider", Context.MODE_PRIVATE);
            libraryChanges = new DocumentsLibraryChanges(preferences.getBoolean("library-dirty", false),
                    dirty -> preferences.edit().putBoolean("library-dirty", dirty).apply());
        }
        return libraryChanges;
    }

    @Override public boolean onCreate() { return true; }

    static DocumentsPathPolicy initializeDocuments(Context context) throws IOException {
        File documents = documentsDirectory(context);
        if (!documents.isDirectory() && !documents.mkdirs()) {
            throw new FileNotFoundException("Could not create Documents folder");
        }
        DocumentsPathPolicy policy = new DocumentsPathPolicy(documents);
        File skins = policy.child(documents, "Skins");
        if (!skins.isDirectory() && !skins.mkdirs()) {
            throw new FileNotFoundException("Could not create Skins folder");
        }
        return policy;
    }

    private DocumentsPathPolicy paths() throws IOException {
        return initializeDocuments(getContext());
    }

    private static FileNotFoundException failure(IOException cause) {
        FileNotFoundException result = new FileNotFoundException(cause.getMessage());
        result.initCause(cause);
        return result;
    }

    @Override public Cursor queryRoots(String[] projection) {
        MatrixCursor cursor = new MatrixCursor(projection != null ? projection : ROOT_COLUMNS);
        cursor.newRow().add(Root.COLUMN_ROOT_ID, DocumentsPathPolicy.ROOT_ID)
                .add(Root.COLUMN_DOCUMENT_ID, DocumentsPathPolicy.ROOT_DOCUMENT_ID)
                .add(Root.COLUMN_TITLE, getContext().getString(R.string.app_name))
                .add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_CREATE | Root.FLAG_SUPPORTS_IS_CHILD | Root.FLAG_LOCAL_ONLY)
                .add(Root.COLUMN_ICON, R.mipmap.ic_launcher)
                .add(Root.COLUMN_AVAILABLE_BYTES, documentsDirectory(getContext()).getParentFile().getUsableSpace())
                .add(Root.COLUMN_MIME_TYPES, "*/*");
        cursor.setNotificationUri(getContext().getContentResolver(), DocumentsContract.buildRootsUri(AUTHORITY));
        return cursor;
    }

    @Override public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        try {
            MatrixCursor cursor = new MatrixCursor(projection != null ? projection : DOCUMENT_COLUMNS);
            addDocument(cursor, paths(), documentId);
            cursor.setNotificationUri(getContext().getContentResolver(), documentUri(documentId));
            return cursor;
        } catch (IOException error) { throw failure(error); }
    }

    @Override public Cursor queryChildDocuments(String parentId, String[] projection, String sortOrder)
            throws FileNotFoundException {
        try {
            DocumentsPathPolicy policy = paths();
            File[] children = policy.resolve(parentId).listFiles();
            if (children == null) throw new FileNotFoundException("Could not list folder");
            MatrixCursor cursor = new MatrixCursor(projection != null ? projection : DOCUMENT_COLUMNS);
            for (File child : children) {
                try { addDocument(cursor, policy, policy.documentId(child)); }
                catch (FileNotFoundException ignored) { /* Removed concurrently or a symlink. */ }
            }
            cursor.setNotificationUri(getContext().getContentResolver(), childrenUri(parentId));
            return cursor;
        } catch (IOException error) { throw failure(error); }
    }

    private void addDocument(MatrixCursor cursor, DocumentsPathPolicy policy, String id) throws IOException {
        File file = policy.resolve(id);
        boolean root = DocumentsPathPolicy.ROOT_DOCUMENT_ID.equals(id);
        int flags = root ? 0 : Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME;
        flags |= file.isDirectory() ? Document.FLAG_DIR_SUPPORTS_CREATE : Document.FLAG_SUPPORTS_WRITE;
        if (policy.isReadOnly(file)) flags = 0;
        cursor.newRow().add(Document.COLUMN_DOCUMENT_ID, id)
                .add(Document.COLUMN_DISPLAY_NAME, root ? getContext().getString(R.string.app_name) : file.getName())
                .add(Document.COLUMN_MIME_TYPE, mimeType(file))
                .add(Document.COLUMN_FLAGS, flags)
                .add(Document.COLUMN_SIZE, file.isDirectory() ? null : file.length())
                .add(Document.COLUMN_LAST_MODIFIED, file.lastModified());
    }

    private static String mimeType(File file) {
        if (file.isDirectory()) return Document.MIME_TYPE_DIR;
        String name = file.getName();
        int dot = name.lastIndexOf('.');
        String type = dot >= 0 ? MimeTypeMap.getSingleton().getMimeTypeFromExtension(
                name.substring(dot + 1).toLowerCase(Locale.ROOT)) : null;
        return type != null ? type : "application/octet-stream";
    }

    @Override public boolean isChildDocument(String parentId, String documentId) {
        try { return paths().isChild(parentId, documentId); }
        catch (IOException error) { return false; }
    }

    @Override public DocumentsContract.Path findDocumentPath(String parentId, String documentId)
            throws FileNotFoundException {
        try {
            DocumentsPathPolicy policy = paths();
            File parent = policy.resolve(parentId != null ? parentId : DocumentsPathPolicy.ROOT_DOCUMENT_ID);
            File child = policy.resolve(documentId);
            if (!child.toPath().startsWith(parent.toPath())) {
                throw new FileNotFoundException("Document is outside the requested parent");
            }
            List<String> ids = new ArrayList<>();
            for (File current = child; ; current = current.getParentFile()) {
                ids.add(policy.documentId(current));
                if (current.equals(parent)) break;
            }
            Collections.reverse(ids);
            return new DocumentsContract.Path(parentId == null ? DocumentsPathPolicy.ROOT_ID : null, ids);
        } catch (IOException error) { throw failure(error); }
    }

    @Override public ParcelFileDescriptor openDocument(String documentId, String mode, CancellationSignal signal)
            throws FileNotFoundException {
        synchronized (DOCUMENT_MUTATION_LOCK) {
            return openDocumentLocked(documentId, mode, signal);
        }
    }

    private ParcelFileDescriptor openDocumentLocked(String documentId, String mode, CancellationSignal signal)
            throws FileNotFoundException {
        if (signal != null) signal.throwIfCanceled();
        try {
            DocumentsPathPolicy policy = paths();
            File file = policy.resolve(documentId);
            if (!file.isFile()) throw new FileNotFoundException("Not a regular file");
            int access = ParcelFileDescriptor.parseMode(mode);
            if ((access & ParcelFileDescriptor.MODE_WRITE_ONLY) == 0) {
                return ParcelFileDescriptor.open(file, access);
            }
            policy.requireWritable(file);
            DocumentsMutationGuard.requireUnreserved(file, false);
            boolean bms = DocumentsPathPolicy.affectsBms(documentId);
            DocumentsLibraryChanges changes = changes(getContext());
            if (bms) changes.writerOpened(SystemClock.elapsedRealtime());
            try {
                return ParcelFileDescriptor.open(file, access, closeHandler, error -> {
                    if (bms) changes.writerClosed(SystemClock.elapsedRealtime());
                    notifyDocument(documentId);
                });
            } catch (IOException | RuntimeException error) {
                if (bms) changes.writerClosed(SystemClock.elapsedRealtime());
                throw error;
            }
        } catch (IOException error) { throw failure(error); }
    }

    @Override public String createDocument(String parentId, String mimeType, String displayName) throws FileNotFoundException {
        synchronized (DOCUMENT_MUTATION_LOCK) {
            return createDocumentLocked(parentId, mimeType, displayName);
        }
    }

    private String createDocumentLocked(String parentId, String mimeType, String displayName) throws FileNotFoundException {
        try {
            DocumentsPathPolicy policy = paths();
            File parent = policy.resolve(parentId);
            if (!parent.isDirectory()) throw new FileNotFoundException("Not a folder");
            policy.requireWritable(parent);
            File file = policy.child(parent, displayName);
            // Check the requested name before suffixing an existing document;
            // reserved root names must not become writable replacement trees.
            policy.requireWritable(file);
            // Never overwrite an existing user file when a manager copies a duplicate name.
            for (int suffix = 1; ; ++suffix) {
                DocumentsMutationGuard.requireUnreserved(file, false);
                boolean created = Document.MIME_TYPE_DIR.equals(mimeType) ? file.mkdir() : file.createNewFile();
                if (created) break;
                if (!file.exists()) throw new IOException("Could not create document");
                int dot = displayName.lastIndexOf('.');
                String base = dot > 0 && !Document.MIME_TYPE_DIR.equals(mimeType) ? displayName.substring(0, dot) : displayName;
                String extension = base.length() < displayName.length() ? displayName.substring(base.length()) : "";
                file = policy.child(parent, base + " (" + suffix + ")" + extension);
                policy.requireWritable(file);
            }
            String id = policy.documentId(file);
            changed(id);
            return id;
        } catch (IOException error) { throw failure(error); }
    }

    @Override public String renameDocument(String documentId, String displayName) throws FileNotFoundException {
        synchronized (DOCUMENT_MUTATION_LOCK) {
            return renameDocumentLocked(documentId, displayName);
        }
    }

    private String renameDocumentLocked(String documentId, String displayName) throws FileNotFoundException {
        requireMutable(documentId);
        try {
            DocumentsPathPolicy policy = paths();
            File source = policy.resolve(documentId);
            File destination = policy.child(source.getParentFile(), displayName);
            policy.requireWritable(source);
            policy.requireWritable(destination);
            DocumentsMutationGuard.requireUnreserved(source, true);
            DocumentsMutationGuard.requireUnreserved(destination, false);
            if (source.equals(destination)) return documentId;
            if (destination.exists()) throw new IOException("A document with that name already exists");
            Files.move(source.toPath(), destination.toPath());
            String renamed = policy.documentId(destination);
            changed(documentId);
            changed(renamed);
            return renamed;
        } catch (IOException error) { throw failure(error); }
    }

    @Override public void deleteDocument(String documentId) throws FileNotFoundException {
        synchronized (DOCUMENT_MUTATION_LOCK) {
            deleteDocumentLocked(documentId);
        }
    }

    private void deleteDocumentLocked(String documentId) throws FileNotFoundException {
        requireMutable(documentId);
        try {
            DocumentsPathPolicy policy = paths();
            File file = policy.resolve(documentId);
            policy.requireWritable(file);
            DocumentsMutationGuard.requireUnreserved(file, true);
            // walkFileTree does not follow symbolic links within a deleted directory.
            Files.walkFileTree(file.toPath(), new SimpleFileVisitor<Path>() {
                @Override public FileVisitResult visitFile(Path file, BasicFileAttributes attrs) throws IOException {
                    Files.delete(file);
                    return FileVisitResult.CONTINUE;
                }
                @Override public FileVisitResult postVisitDirectory(Path directory, IOException error) throws IOException {
                    if (error != null) throw error;
                    Files.delete(directory);
                    return FileVisitResult.CONTINUE;
                }
            });
        } catch (IOException error) { throw failure(error); }
        finally { changed(documentId); } // Also refresh a partially completed deletion.
    }

    private static void requireMutable(String id) throws FileNotFoundException {
        if (DocumentsPathPolicy.ROOT_DOCUMENT_ID.equals(id)) throw new FileNotFoundException("Cannot modify Documents root");
    }

    private static Uri documentUri(String id) { return DocumentsContract.buildDocumentUri(AUTHORITY, id); }
    private static Uri childrenUri(String id) { return DocumentsContract.buildChildDocumentsUri(AUTHORITY, id); }

    private void changed(String id) {
        if (DocumentsPathPolicy.affectsBms(id)) changes(getContext()).changed(SystemClock.elapsedRealtime());
        notifyDocument(id);
    }

    private void notifyDocument(String id) {
        getContext().getContentResolver().notifyChange(documentUri(id), null);
        int slash = id.lastIndexOf('/');
        String parentId = slash < 0 ? DocumentsPathPolicy.ROOT_DOCUMENT_ID : id.substring(0, slash);
        getContext().getContentResolver().notifyChange(childrenUri(parentId), null);
    }
}
