package com.snurhythm.asobmashow;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

final class SafSkinDirectorySource implements SkinDirectoryImport.Source {
    interface IoControl {
        void checkpoint() throws IOException;
        void descriptor(ParcelFileDescriptor descriptor) throws IOException;
    }
    private static final String[] COLUMNS = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME,
            Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE};
    private final ContentResolver resolver;
    private final Uri tree;
    private final CancellationSignal cancellation;
    private final IoControl control;
    SafSkinDirectorySource(ContentResolver resolver, Uri tree, CancellationSignal cancellation, IoControl control) {
        this.resolver = resolver; this.tree = tree; this.cancellation = cancellation; this.control = control;
    }
    @Override public void checkpoint() throws IOException { control.checkpoint(); cancellation.throwIfCanceled(); }
    @Override public SkinDirectoryImport.Entry root() throws IOException {
        checkpoint();
        try (Cursor cursor = resolver.query(document(DocumentsContract.getTreeDocumentId(tree)),
                COLUMNS, null, null, null, cancellation)) {
            if (cursor == null || !cursor.moveToFirst()) throw new IOException("Could not read selected folder.");
            requireComplete(cursor);
            SkinDirectoryImport.Entry result = entry(cursor);
            if (cursor.moveToNext()) throw new IOException("Ambiguous folder document.");
            requireComplete(cursor);
            return result;
        }
    }
    @Override public List<SkinDirectoryImport.Entry> children(SkinDirectoryImport.Entry directory, long remaining)
            throws IOException {
        checkpoint();
        List<SkinDirectoryImport.Entry> result = new ArrayList<>();
        try (Cursor cursor = resolver.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, directory.id),
                COLUMNS, null, null, null, cancellation)) {
            if (cursor == null) throw new IOException("Could not read selected folder.");
            requireComplete(cursor);
            while (cursor.moveToNext()) {
                checkpoint();
                if (result.size() >= remaining) throw new IOException("Folder exceeds the skin entry limit.");
                result.add(entry(cursor));
            }
            requireComplete(cursor);
        }
        return result;
    }
    @Override public InputStream open(SkinDirectoryImport.Entry file) throws IOException {
        checkpoint();
        ParcelFileDescriptor descriptor = resolver.openFileDescriptor(document(file.id), "r", cancellation);
        if (descriptor == null) throw new IOException("Could not open skin file.");
        try {
            control.descriptor(descriptor);
            checkpoint();
            return new ParcelFileDescriptor.AutoCloseInputStream(descriptor);
        } catch (IOException | RuntimeException e) {
            descriptor.close();
            throw e;
        }
    }
    private Uri document(String id) { return DocumentsContract.buildDocumentUriUsingTree(tree, id); }
    private static void requireComplete(Cursor cursor) throws IOException {
        Bundle extras = cursor.getExtras();
        if (extras != null && (extras.getBoolean(DocumentsContract.EXTRA_LOADING, false)
                || extras.containsKey(DocumentsContract.EXTRA_ERROR)))
            throw new IOException("Folder listing is incomplete. Wait for the storage provider and retry.");
    }
    private static SkinDirectoryImport.Entry entry(Cursor cursor) {
        int size = cursor.getColumnIndex(Document.COLUMN_SIZE);
        return new SkinDirectoryImport.Entry(cursor.getString(cursor.getColumnIndexOrThrow(Document.COLUMN_DOCUMENT_ID)),
                cursor.getString(cursor.getColumnIndexOrThrow(Document.COLUMN_DISPLAY_NAME)),
                Document.MIME_TYPE_DIR.equals(cursor.getString(cursor.getColumnIndexOrThrow(Document.COLUMN_MIME_TYPE))),
                size < 0 || cursor.isNull(size) ? -1 : cursor.getLong(size));
    }
}
