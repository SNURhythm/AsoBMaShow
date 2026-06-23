package com.snurhythm.asobmashow;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.OpenableColumns;
import android.provider.Settings;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Locale;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

public class AsoBMaShowActivity extends SDLActivity {
    private static final int REQUEST_OPEN_TREE = 0x41534f42;
    private static final int REQUEST_OPEN_ARCHIVE = 0x41534f44;
    private static final int REQUEST_MANAGE_EXTERNAL_STORAGE = 0x41534f45;
    private static final String ERROR_PREFIX = "__ERROR__:";

    private final Object pickerLock = new Object();
    private CountDownLatch pickerLatch;
    private final AtomicReference<String> pickerResult = new AtomicReference<>("");
    private final Object archivePickerLock = new Object();
    private CountDownLatch archivePickerLatch;
    private final AtomicReference<Uri> archivePickerUri = new AtomicReference<>(null);
    private final AtomicReference<String> archivePickerName = new AtomicReference<>("");
    private final Object manageStorageLock = new Object();
    private CountDownLatch manageStorageLatch;
    private final Object pendingArchiveImportLock = new Object();
    private String pendingArchiveImportPath = "";
    private String pendingArchiveImportError = "";
    private boolean pendingArchiveImportCopyRunning = false;
    private final ConcurrentHashMap<String, String> documentIdCache = new ConcurrentHashMap<>();
    private final ConcurrentHashMap<String, String> transientDocumentIdCache = new ConcurrentHashMap<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        super.onCreate(savedInstanceState);
        handleArchiveImportIntent(getIntent());
    }

    @Override
    protected void onResume() {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        super.onResume();
        finishManageStorageRequest();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleArchiveImportIntent(intent);
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

    public String getInternalFilesDirPath() {
        return getFilesDir().getAbsolutePath();
    }

    public String ensureManageExternalStorageAccess() {
        if (!BuildConfig.ASOBMSHOW_MANAGE_EXTERNAL_STORAGE) {
            return "0";
        }
        if (hasManageExternalStorageAccess()) {
            return "1";
        }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return ERROR_PREFIX + "All-files permission cannot block the UI thread.";
        }

        CountDownLatch latch;
        synchronized (manageStorageLock) {
            if (manageStorageLatch != null) {
                return ERROR_PREFIX + "All-files permission request is already open.";
            }
            latch = new CountDownLatch(1);
            manageStorageLatch = latch;
        }

        runOnUiThread(() -> {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
            intent.setData(Uri.parse("package:" + getPackageName()));
            try {
                startActivityForResult(intent, REQUEST_MANAGE_EXTERNAL_STORAGE);
            } catch (Exception e) {
                try {
                    startActivityForResult(
                            new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION),
                            REQUEST_MANAGE_EXTERNAL_STORAGE);
                } catch (Exception ignored) {
                    finishManageStorageRequest();
                }
            }
        });

        try {
            latch.await();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return ERROR_PREFIX + "All-files permission request was interrupted.";
        }

        synchronized (manageStorageLock) {
            manageStorageLatch = null;
        }
        return hasManageExternalStorageAccess() ? "1" : "0";
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
                String directPath = directPathForTree(treeUri);
                pickerResult.set(treeUri.toString() + "\n" + displayName + "\n" + directPath);
            } else {
                pickerResult.set(ERROR_PREFIX + "Folder selection was cancelled.");
            }
            finishPicker();
            return;
        }
        if (requestCode == REQUEST_OPEN_ARCHIVE) {
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
                Uri archiveUri = data.getData();
                archivePickerUri.set(archiveUri);
                archivePickerName.set(displayNameForUri(archiveUri));
            } else {
                archivePickerUri.set(null);
                archivePickerName.set("");
            }
            finishArchivePicker();
            return;
        }
        if (requestCode == REQUEST_MANAGE_EXTERNAL_STORAGE) {
            finishManageStorageRequest();
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

    public String clearTransientTreeFileCache() {
        transientDocumentIdCache.clear();
        return "OK";
    }

    public String cacheTreeDirectory(String treeUriText, String relativeDir) {
        try {
            Uri treeUri = Uri.parse(treeUriText);
            String normalizedDir = normalizeRelativePath(relativeDir);
            String parentDocumentId = resolveDocumentId(treeUri, normalizedDir);
            if (parentDocumentId == null) {
                return ERROR_PREFIX + "Directory not found: " + normalizedDir;
            }
            return Integer.toString(cacheDirectChildren(treeUri, parentDocumentId,
                    normalizedDir, transientDocumentIdCache));
        } catch (Exception e) {
            return ERROR_PREFIX + e.getMessage();
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

    public String pickArchiveForImport() {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return ERROR_PREFIX + "Archive picker cannot block the UI thread.";
        }

        CountDownLatch latch;
        synchronized (archivePickerLock) {
            if (archivePickerLatch != null) {
                return ERROR_PREFIX + "Archive picker is already open.";
            }
            archivePickerUri.set(null);
            archivePickerName.set("");
            latch = new CountDownLatch(1);
            archivePickerLatch = latch;
        }

        runOnUiThread(() -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            intent.putExtra(Intent.EXTRA_MIME_TYPES, archiveMimeTypes());
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            try {
                startActivityForResult(intent, REQUEST_OPEN_ARCHIVE);
            } catch (Exception e) {
                archivePickerUri.set(null);
                archivePickerName.set("");
                finishArchivePicker();
            }
        });

        try {
            latch.await();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return ERROR_PREFIX + "Archive picker was interrupted.";
        }

        synchronized (archivePickerLock) {
            archivePickerLatch = null;
        }

        Uri uri = archivePickerUri.get();
        if (uri == null) {
            return ERROR_PREFIX + "Archive selection was cancelled.";
        }
        try {
            return copyArchiveUriToInternalStorage(uri, archivePickerName.get());
        } catch (Exception e) {
            return ERROR_PREFIX + e.getMessage();
        }
    }

    public String consumePendingArchiveImport() {
        synchronized (pendingArchiveImportLock) {
            if (!pendingArchiveImportPath.isEmpty()) {
                String path = pendingArchiveImportPath;
                pendingArchiveImportPath = "";
                return path;
            }
            if (!pendingArchiveImportError.isEmpty()) {
                String error = pendingArchiveImportError;
                pendingArchiveImportError = "";
                return ERROR_PREFIX + error;
            }
            return "";
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

    private void finishArchivePicker() {
        synchronized (archivePickerLock) {
            if (archivePickerLatch != null) {
                archivePickerLatch.countDown();
            }
        }
    }

    private void finishManageStorageRequest() {
        synchronized (manageStorageLock) {
            if (manageStorageLatch != null) {
                manageStorageLatch.countDown();
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
                boolean isDirectory = Document.MIME_TYPE_DIR.equals(mimeType);
                boolean isChart = isChartFile(name);
                if (isDirectory || isChart) {
                    documentIdCache.put(cacheKey(treeUri, relativePath), documentId);
                }
                if (isDirectory) {
                    listChartFilesRecursive(treeUri, documentId, relativePath,
                            syntheticRoot, output);
                } else if (isChart) {
                    output.append(syntheticRoot).append('/').append(relativePath).append('\n');
                }
            }
        }
    }

    private boolean isChartFile(String name) {
        String lower = name.toLowerCase(Locale.ROOT);
        return lower.endsWith(".bms") || lower.endsWith(".bme") || lower.endsWith(".bml");
    }

    private boolean hasManageExternalStorageAccess() {
        return BuildConfig.ASOBMSHOW_MANAGE_EXTERNAL_STORAGE
                && (Build.VERSION.SDK_INT < Build.VERSION_CODES.R
                || Environment.isExternalStorageManager());
    }

    private String directPathForTree(Uri treeUri) {
        if (!hasManageExternalStorageAccess()
                || !"com.android.externalstorage.documents".equals(treeUri.getAuthority())) {
            return "";
        }
        try {
            String treeDocumentId = DocumentsContract.getTreeDocumentId(treeUri);
            int separator = treeDocumentId.indexOf(':');
            if (separator < 0) {
                return "";
            }
            String volume = treeDocumentId.substring(0, separator);
            String relative = normalizeRelativePath(treeDocumentId.substring(separator + 1));
            File volumeRoot;
            if ("primary".equalsIgnoreCase(volume)) {
                volumeRoot = Environment.getExternalStorageDirectory();
            } else {
                volumeRoot = new File("/storage", volume);
            }
            return relative.isEmpty()
                    ? volumeRoot.getAbsolutePath()
                    : new File(volumeRoot, relative).getAbsolutePath();
        } catch (Exception e) {
            return "";
        }
    }

    private String resolveDocumentId(Uri treeUri, String relativePath) {
        relativePath = normalizeRelativePath(relativePath);
        if (relativePath == null || relativePath.isEmpty()) {
            return DocumentsContract.getTreeDocumentId(treeUri);
        }
        String cached = transientDocumentIdCache.get(cacheKey(treeUri, relativePath));
        if (cached != null) {
            return cached;
        }
        cached = documentIdCache.get(cacheKey(treeUri, relativePath));
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
            transientDocumentIdCache.put(cacheKey(treeUri, currentPath.toString()), nextDocumentId);
            currentDocumentId = nextDocumentId;
        }
        return currentDocumentId;
    }

    private int cacheDirectChildren(Uri treeUri, String parentDocumentId,
                                    String relativeDir,
                                    ConcurrentHashMap<String, String> targetCache) {
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(
                treeUri, parentDocumentId);
        String[] columns = new String[] {
                Document.COLUMN_DOCUMENT_ID,
                Document.COLUMN_DISPLAY_NAME
        };
        int count = 0;
        try (Cursor cursor = getContentResolver().query(
                childrenUri, columns, null, null, null)) {
            if (cursor == null) {
                return 0;
            }
            int idColumn = cursor.getColumnIndexOrThrow(Document.COLUMN_DOCUMENT_ID);
            int nameColumn = cursor.getColumnIndexOrThrow(Document.COLUMN_DISPLAY_NAME);
            while (cursor.moveToNext()) {
                String documentId = cursor.getString(idColumn);
                String name = cursor.getString(nameColumn);
                if (name == null || name.isEmpty()) {
                    continue;
                }
                String relativePath = relativeDir.isEmpty() ? name : relativeDir + "/" + name;
                targetCache.put(cacheKey(treeUri, relativePath), documentId);
                count++;
            }
        }
        return count;
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

    private String normalizeRelativePath(String relativePath) {
        if (relativePath == null) {
            return "";
        }
        String normalized = relativePath.replace('\\', '/');
        while (normalized.startsWith("/")) {
            normalized = normalized.substring(1);
        }
        return normalized;
    }

    private String cacheKey(Uri treeUri, String relativePath) {
        return treeUri.toString() + "\n" + relativePath;
    }

    private String[] archiveMimeTypes() {
        return new String[] {
                "application/zip",
                "application/x-zip-compressed",
                "application/x-7z-compressed",
                "application/vnd.rar",
                "application/x-rar-compressed",
                "application/x-lha",
                "application/x-lzh-compressed",
                "application/x-tar",
                "application/gzip",
                "application/x-gzip",
                "application/octet-stream"
        };
    }

    private void handleArchiveImportIntent(Intent intent) {
        Uri archiveUri = archiveUriFromIntent(intent);
        if (archiveUri == null) {
            return;
        }
        String displayName = displayNameForUri(archiveUri);
        synchronized (pendingArchiveImportLock) {
            if (pendingArchiveImportCopyRunning) {
                pendingArchiveImportError = "Archive import is already running.";
                return;
            }
            pendingArchiveImportCopyRunning = true;
            pendingArchiveImportError = "";
        }
        new Thread(() -> {
            String path = "";
            String error = "";
            try {
                path = copyArchiveUriToInternalStorage(archiveUri, displayName);
            } catch (Exception e) {
                error = e.getMessage() == null ? "Could not import archive." : e.getMessage();
            }
            synchronized (pendingArchiveImportLock) {
                pendingArchiveImportPath = path;
                pendingArchiveImportError = error;
                pendingArchiveImportCopyRunning = false;
            }
        }, "AsoBMaShowArchiveImport").start();
    }

    private Uri archiveUriFromIntent(Intent intent) {
        if (intent == null) {
            return null;
        }
        String action = intent.getAction();
        if (Intent.ACTION_VIEW.equals(action)) {
            return intent.getData();
        }
        if (Intent.ACTION_SEND.equals(action)) {
            Object stream = intent.getParcelableExtra(Intent.EXTRA_STREAM);
            return stream instanceof Uri ? (Uri) stream : null;
        }
        return null;
    }

    private String displayNameForUri(Uri uri) {
        if (uri == null) {
            return "imported-archive";
        }
        if (ContentResolver.SCHEME_CONTENT.equals(uri.getScheme())) {
            try (Cursor cursor = getContentResolver().query(
                    uri, new String[] { OpenableColumns.DISPLAY_NAME },
                    null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) {
                    int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                    if (column >= 0) {
                        String name = cursor.getString(column);
                        if (name != null && !name.isEmpty()) {
                            return name;
                        }
                    }
                }
            } catch (Exception ignored) {
            }
        }
        String last = uri.getLastPathSegment();
        return last == null || last.isEmpty() ? "imported-archive" : last;
    }

    private String copyArchiveUriToInternalStorage(Uri uri, String displayName) throws Exception {
        File directory = new File(getFilesDir(), "archive_imports/inbox");
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new Exception("Could not create archive import folder.");
        }
        String safeName = sanitizeFileName(displayName);
        File output = uniqueFile(directory, safeName);
        try (InputStream input = getContentResolver().openInputStream(uri);
             FileOutputStream outputStream = new FileOutputStream(output)) {
            if (input == null) {
                throw new Exception("Could not open archive import.");
            }
            byte[] buffer = new byte[1024 * 1024];
            while (true) {
                int read = input.read(buffer);
                if (read < 0) {
                    break;
                }
                outputStream.write(buffer, 0, read);
            }
        }
        return output.getAbsolutePath();
    }

    private File uniqueFile(File directory, String fileName) {
        File candidate = new File(directory, fileName);
        if (!candidate.exists()) {
            return candidate;
        }
        String base = fileName;
        String extension = "";
        int dot = fileName.lastIndexOf('.');
        if (dot > 0) {
            base = fileName.substring(0, dot);
            extension = fileName.substring(dot);
        }
        for (int i = 2; i < 10000; i++) {
            candidate = new File(directory, base + " " + i + extension);
            if (!candidate.exists()) {
                return candidate;
            }
        }
        return new File(directory, base + " " + System.currentTimeMillis() + extension);
    }

    private String sanitizeFileName(String name) {
        if (name == null || name.isEmpty()) {
            name = "imported-archive";
        }
        String sanitized = name.replace('/', '_').replace('\\', '_').replace('\0', '_');
        while (sanitized.startsWith(".")) {
            sanitized = sanitized.substring(1);
        }
        return sanitized.isEmpty() ? "imported-archive" : sanitized;
    }
}
