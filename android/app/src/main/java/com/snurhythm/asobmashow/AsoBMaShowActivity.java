package com.snurhythm.asobmashow;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;

import org.libsdl.app.SDLActivity;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.Locale;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

public class AsoBMaShowActivity extends SDLActivity {
    private static final int REQUEST_OPEN_TREE = 0x41534f42;
    private static final String ERROR_PREFIX = "__ERROR__:";

    private final Object pickerLock = new Object();
    private CountDownLatch pickerLatch;
    private final AtomicReference<String> pickerResult = new AtomicReference<>("");
    private final ConcurrentHashMap<String, String> documentIdCache = new ConcurrentHashMap<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        super.onCreate(savedInstanceState);
    }

    @Override
    protected void onResume() {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        super.onResume();
    }

    @Override
    public void setOrientationBis(int width, int height, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
                "SDL2",
                "SDL2_ttf",
                "main"
        };
    }

    public String pickChartFolder() {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return ERROR_PREFIX + "Folder picker cannot block the UI thread.";
        }

        CountDownLatch latch;
        synchronized (pickerLock) {
            if (pickerLatch != null) {
                return ERROR_PREFIX + "Folder picker is already open.";
            }
            pickerResult.set("");
            latch = new CountDownLatch(1);
            pickerLatch = latch;
        }

        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                    | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
            try {
                startActivityForResult(intent, REQUEST_OPEN_TREE);
            } catch (Exception e) {
                pickerResult.set(ERROR_PREFIX + e.getMessage());
                finishPicker();
            }
        });

        try {
            latch.await();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return ERROR_PREFIX + "Folder picker was interrupted.";
        }

        synchronized (pickerLock) {
            pickerLatch = null;
        }
        return pickerResult.get();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQUEST_OPEN_TREE) {
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
                Uri treeUri = data.getData();
                int flags = data.getFlags()
                        & (Intent.FLAG_GRANT_READ_URI_PERMISSION
                        | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                try {
                    getContentResolver().takePersistableUriPermission(
                            treeUri, flags & Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (Exception ignored) {
                    // Some providers grant transient access only; keep using the URI for this run.
                }
                String displayName = displayNameForTree(treeUri);
                pickerResult.set(treeUri.toString() + "\n" + displayName);
            } else {
                pickerResult.set(ERROR_PREFIX + "Folder selection was cancelled.");
            }
            finishPicker();
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    public String listChartFiles(String treeUriText, String syntheticRoot) {
        try {
            Uri treeUri = Uri.parse(treeUriText);
            String rootDocumentId = DocumentsContract.getTreeDocumentId(treeUri);
            StringBuilder output = new StringBuilder(64 * 1024);
            listChartFilesRecursive(treeUri, rootDocumentId, "", syntheticRoot, output);
            return output.length() == 0 ? "\n" : output.toString();
        } catch (Exception e) {
            return ERROR_PREFIX + e.getMessage();
        }
    }

    public String existsTreeFile(String treeUriText, String relativePath) {
        try {
            Uri treeUri = Uri.parse(treeUriText);
            String documentId = resolveDocumentId(treeUri, relativePath);
            return documentId == null ? "0" : "1";
        } catch (Exception e) {
            return ERROR_PREFIX + e.getMessage();
        }
    }

    public byte[] readTreeFile(String treeUriText, String relativePath) {
        try {
            Uri treeUri = Uri.parse(treeUriText);
            String documentId = resolveDocumentId(treeUri, relativePath);
            if (documentId == null) {
                return null;
            }
            Uri documentUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId);
            try (InputStream input = getContentResolver().openInputStream(documentUri);
                 ByteArrayOutputStream output = new ByteArrayOutputStream()) {
                if (input == null) {
                    return null;
                }
                byte[] buffer = new byte[64 * 1024];
                int read;
                while ((read = input.read(buffer)) >= 0) {
                    output.write(buffer, 0, read);
                }
                return output.toByteArray();
            }
        } catch (Exception e) {
            return null;
        }
    }

    public int openTreeFileDescriptor(String treeUriText, String relativePath) {
        try {
            Uri treeUri = Uri.parse(treeUriText);
            String documentId = resolveDocumentId(treeUri, relativePath);
            if (documentId == null) {
                return -1;
            }
            Uri documentUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId);
            ParcelFileDescriptor fd =
                    getContentResolver().openFileDescriptor(documentUri, "r");
            return fd == null ? -1 : fd.detachFd();
        } catch (Exception e) {
            return -1;
        }
    }

    public String openExternalUrl(String url) {
        try {
            Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(intent);
            return "OK";
        } catch (Exception e) {
            return ERROR_PREFIX + e.getMessage();
        }
    }

    private void finishPicker() {
        synchronized (pickerLock) {
            if (pickerLatch != null) {
                pickerLatch.countDown();
            }
        }
    }

    private String displayNameForTree(Uri treeUri) {
        String treeDocumentId = DocumentsContract.getTreeDocumentId(treeUri);
        int separator = treeDocumentId.lastIndexOf(':');
        String name = separator >= 0 ? treeDocumentId.substring(separator + 1) : treeDocumentId;
        if (name == null || name.isEmpty()) {
            return "Library";
        }
        return name.replace('/', '_').replace('\\', '_');
    }

    private void listChartFilesRecursive(Uri treeUri, String parentDocumentId,
                                         String relativeDir, String syntheticRoot,
                                         StringBuilder output) {
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(
                treeUri, parentDocumentId);
        String[] columns = new String[] {
                Document.COLUMN_DOCUMENT_ID,
                Document.COLUMN_DISPLAY_NAME,
                Document.COLUMN_MIME_TYPE
        };
        try (Cursor cursor = getContentResolver().query(
                childrenUri, columns, null, null, null)) {
            if (cursor == null) {
                return;
            }
            int idColumn = cursor.getColumnIndexOrThrow(Document.COLUMN_DOCUMENT_ID);
            int nameColumn = cursor.getColumnIndexOrThrow(Document.COLUMN_DISPLAY_NAME);
            int mimeColumn = cursor.getColumnIndexOrThrow(Document.COLUMN_MIME_TYPE);
            while (cursor.moveToNext()) {
                String documentId = cursor.getString(idColumn);
                String name = cursor.getString(nameColumn);
                String mimeType = cursor.getString(mimeColumn);
                if (name == null || name.isEmpty()) {
                    continue;
                }
                String relativePath = relativeDir.isEmpty() ? name : relativeDir + "/" + name;
                documentIdCache.put(cacheKey(treeUri, relativePath), documentId);
                if (Document.MIME_TYPE_DIR.equals(mimeType)) {
                    listChartFilesRecursive(treeUri, documentId, relativePath,
                            syntheticRoot, output);
                } else if (isChartFile(name)) {
                    output.append(syntheticRoot).append('/').append(relativePath).append('\n');
                }
            }
        }
    }

    private boolean isChartFile(String name) {
        String lower = name.toLowerCase(Locale.ROOT);
        return lower.endsWith(".bms") || lower.endsWith(".bme") || lower.endsWith(".bml");
    }

    private String resolveDocumentId(Uri treeUri, String relativePath) {
        if (relativePath == null || relativePath.isEmpty()) {
            return DocumentsContract.getTreeDocumentId(treeUri);
        }
        String cached = documentIdCache.get(cacheKey(treeUri, relativePath));
        if (cached != null) {
            return cached;
        }

        String currentDocumentId = DocumentsContract.getTreeDocumentId(treeUri);
        StringBuilder currentPath = new StringBuilder();
        String[] parts = relativePath.split("/");
        for (String part : parts) {
            if (part.isEmpty()) {
                continue;
            }
            String nextDocumentId = findChildDocumentId(treeUri, currentDocumentId, part);
            if (nextDocumentId == null) {
                return null;
            }
            if (currentPath.length() > 0) {
                currentPath.append('/');
            }
            currentPath.append(part);
            documentIdCache.put(cacheKey(treeUri, currentPath.toString()), nextDocumentId);
            currentDocumentId = nextDocumentId;
        }
        return currentDocumentId;
    }

    private String findChildDocumentId(Uri treeUri, String parentDocumentId, String childName) {
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(
                treeUri, parentDocumentId);
        String[] columns = new String[] {
                Document.COLUMN_DOCUMENT_ID,
                Document.COLUMN_DISPLAY_NAME
        };
        try (Cursor cursor = getContentResolver().query(
                childrenUri, columns, null, null, null)) {
            if (cursor == null) {
                return null;
            }
            int idColumn = cursor.getColumnIndexOrThrow(Document.COLUMN_DOCUMENT_ID);
            int nameColumn = cursor.getColumnIndexOrThrow(Document.COLUMN_DISPLAY_NAME);
            while (cursor.moveToNext()) {
                String name = cursor.getString(nameColumn);
                if (childName.equals(name)) {
                    return cursor.getString(idColumn);
                }
            }
        }
        return null;
    }

    private String cacheKey(Uri treeUri, String relativePath) {
        return treeUri.toString() + "\n" + relativePath;
    }
}
