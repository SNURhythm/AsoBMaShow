package com.snurhythm.asobmashow;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.util.Log;

import java.io.InputStream;
import java.io.OutputStream;
import java.util.UUID;

/** Runs under the test APK's separate UID, with only the launched explorer's URI grants. */
public final class DocumentsExplorerProbeActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        Uri tree = getIntent().getClipData().getItemAt(0).getUri();
        Uri created = null;
        int result = RESULT_CANCELED;
        int access = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
        try {
            getContentResolver().takePersistableUriPermission(tree, access);
            Uri bms = getIntent().getData();
            Uri root = DocumentsContract.buildDocumentUriUsingTree(tree,
                    DocumentsContract.getTreeDocumentId(tree));
            for (Uri directory : new Uri[]{bms, root}) {
                Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree,
                        DocumentsContract.getDocumentId(directory));
                try (Cursor cursor = getContentResolver().query(children, null, null, null, null)) {
                    if (cursor == null) throw new AssertionError("Cannot browse " + directory);
                    cursor.getCount();
                }
            }
            created = DocumentsContract.createDocument(getContentResolver(), root,
                    "application/octet-stream", "ExplorerGrantTest-" + UUID.randomUUID());
            if (created == null) throw new AssertionError("Cannot create in Documents root");
            try (OutputStream stream = getContentResolver().openOutputStream(created)) { stream.write(42); }
            try (InputStream stream = getContentResolver().openInputStream(created)) {
                if (stream.read() != 42) throw new AssertionError("Cannot read explorer write");
            }
            if (!DocumentsContract.deleteDocument(getContentResolver(), created)) {
                throw new AssertionError("Cannot delete explorer fixture");
            }
            created = null;
            result = RESULT_OK;
            Log.i("DocumentsExplorerProbe", "PASS separate-UID BMS/root browse, persist, create/read/write/delete");
        } catch (Exception | AssertionError error) {
            Log.e("DocumentsExplorerProbe", "FAIL explorer grant", error);
        } finally {
            try {
                if (created != null) DocumentsContract.deleteDocument(getContentResolver(), created);
                getContentResolver().releasePersistableUriPermission(tree, access);
            } catch (Exception error) {
                result = RESULT_CANCELED;
                Log.e("DocumentsExplorerProbe", "FAIL cleanup", error);
            }
            android.os.ResultReceiver receiver = getIntent().getParcelableExtra("explorerResult");
            if (receiver != null) receiver.send(result, Bundle.EMPTY);
            finish();
        }
    }
}
