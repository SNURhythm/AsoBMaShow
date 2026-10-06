package com.snurhythm.asobmashow;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;

import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

/** SAF access stays on the import worker; the player reads the resulting native files. */
final class SafChartFolderSource implements ChartFolderImport.Source {
    private static final String[] COLUMNS = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME,
            Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED};
    private final ContentResolver resolver;
    private final Uri tree;
    private final ChartImportCopyControl control;

    SafChartFolderSource(ContentResolver resolver, Uri tree, ChartImportCopyControl control) {
        this.resolver = resolver;
        this.tree = tree;
        this.control = control;
    }

    @Override public ChartFolderImport.Entry root() throws IOException {
        control.checkpoint();
        try (Cursor cursor = resolver.query(document(DocumentsContract.getTreeDocumentId(tree)),
                COLUMNS, null, null, null)) {
            if (cursor == null || !cursor.moveToFirst()) throw new IOException("Could not read selected folder.");
            requireComplete(cursor);
            ChartFolderImport.Entry result = entry(cursor);
            requireComplete(cursor);
            return result;
        }
    }

    @Override public List<ChartFolderImport.Entry> children(ChartFolderImport.Entry directory) throws IOException {
        List<ChartFolderImport.Entry> result = new ArrayList<>();
        control.checkpoint();
        try (Cursor cursor = resolver.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, directory.id),
                COLUMNS, null, null, null)) {
            if (cursor == null) throw new IOException("Could not read selected folder.");
            requireComplete(cursor);
            while (true) {
                control.checkpoint();
                if (!cursor.moveToNext()) break;
                result.add(entry(cursor));
            }
            requireComplete(cursor);
        }
        return result;
    }

    @Override public InputStream open(ChartFolderImport.Entry file) throws IOException {
        control.checkpoint();
        return resolver.openInputStream(document(file.id));
    }

    @Override public void delete(ChartFolderImport.Entry directory) throws IOException {
        // The engine validates immediately before deletion; do not pause after that snapshot.
        if (!DocumentsContract.deleteDocument(resolver, document(directory.id))) {
            throw new IOException("Could not remove original folder: " + directory.name);
        }
    }

    private static void requireComplete(Cursor cursor) throws IOException {
        android.os.Bundle extras = cursor.getExtras();
        if (extras != null && (extras.getBoolean(DocumentsContract.EXTRA_LOADING, false)
                || extras.containsKey(DocumentsContract.EXTRA_ERROR))) {
            throw new IOException("Folder listing is incomplete. Wait for the storage provider to finish loading and retry.");
        }
    }

    private Uri document(String id) { return DocumentsContract.buildDocumentUriUsingTree(tree, id); }

    private static ChartFolderImport.Entry entry(Cursor cursor) {
        return new ChartFolderImport.Entry(cursor.getString(cursor.getColumnIndexOrThrow(Document.COLUMN_DOCUMENT_ID)),
                cursor.getString(cursor.getColumnIndexOrThrow(Document.COLUMN_DISPLAY_NAME)),
                Document.MIME_TYPE_DIR.equals(cursor.getString(cursor.getColumnIndexOrThrow(Document.COLUMN_MIME_TYPE))),
                optionalLong(cursor, Document.COLUMN_SIZE), optionalLong(cursor, Document.COLUMN_LAST_MODIFIED));
    }

    private static long optionalLong(Cursor cursor, String column) {
        int index = cursor.getColumnIndex(column);
        return index < 0 || cursor.isNull(index) ? -1 : Math.max(-1, cursor.getLong(index));
    }
}
