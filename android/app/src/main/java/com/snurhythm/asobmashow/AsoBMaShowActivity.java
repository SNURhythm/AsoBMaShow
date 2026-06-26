package com.snurhythm.asobmashow;

import android.app.Activity;
import android.app.NotificationManager;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.AudioAttributes;
import android.media.MediaDescription;
import android.media.MediaMetadata;
import android.media.MediaPlayer;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
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

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Locale;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

public class AsoBMaShowActivity extends SDLActivity {
    private static final int REQUEST_OPEN_TREE = 0x41534f42;
    private static final int REQUEST_OPEN_ARCHIVE = 0x41534f44;
    private static final int REQUEST_MANAGE_EXTERNAL_STORAGE = 0x41534f45;
    private static final int REQUEST_OPEN_IMPORT_FOLDER = 0x41534f46;
    private static final String ERROR_PREFIX = "__ERROR__:";
    private static final String PENDING_IMPORT_RESULT = "__PENDING_ARCHIVE_IMPORT__";
    private static final int MAX_TEXT_DOWNLOAD_BYTES = 16 * 1024 * 1024;
    private static final long NATIVE_MUSIC_UNKNOWN_QUEUE_ID = -1L;

    private final Object pickerLock = new Object();
    private CountDownLatch pickerLatch;
    private final AtomicReference<String> pickerResult = new AtomicReference<>("");
    private final Object archivePickerLock = new Object();
    private CountDownLatch archivePickerLatch;
    private final AtomicReference<Uri> archivePickerUri = new AtomicReference<>(null);
    private final AtomicReference<String> archivePickerName = new AtomicReference<>("");
    private final AtomicReference<Boolean> archivePickerTree = new AtomicReference<>(false);
    private final AtomicReference<String> archivePickerError = new AtomicReference<>("");
    private final Object manageStorageLock = new Object();
    private CountDownLatch manageStorageLatch;
    private final Object pendingArchiveImportLock = new Object();
    private final ArrayDeque<PendingImportRequest> pendingArchiveImportRequests =
            new ArrayDeque<>();
    private final ArrayDeque<String> pendingArchiveImportResults = new ArrayDeque<>();
    private boolean pendingArchiveImportCopyRunning = false;
    private final ConcurrentHashMap<String, String> documentIdCache = new ConcurrentHashMap<>();
    private final ConcurrentHashMap<String, String> transientDocumentIdCache = new ConcurrentHashMap<>();
    private final Object nativeMusicLock = new Object();
    private MediaPlayer nativeMusicPlayer;
    private MediaSession nativeMusicSession;
    private String nativeMusicTitle = "AsoBMaShow";
    private String nativeMusicArtist = "AsoBMaShow";
    private String nativeMusicAlbum = "";
    private String nativeMusicArtworkPath = "";
    private Bitmap nativeMusicArtwork;
    private long nativeMusicDurationMicros = 0;
    private String nativeMusicQueueTitle = "";
    private final ArrayList<MediaSession.QueueItem> nativeMusicQueue = new ArrayList<>();
    private long nativeMusicActiveQueueItemId = NATIVE_MUSIC_UNKNOWN_QUEUE_ID;

    private static class PendingImportRequest {
        final Uri uri;
        final String displayName;
        final boolean isTree;

        PendingImportRequest(Uri uri, String displayName, boolean isTree) {
            this.uri = uri;
            this.displayName = displayName;
            this.isTree = isTree;
        }
    }

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
    protected void onDestroy() {
        synchronized (nativeMusicLock) {
            releaseNativeMusicPlayerLocked();
        }
        super.onDestroy();
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

    private static native void nativeDownloadUrlToFileProgress(long progressToken,
                                                               long downloadedBytes,
                                                               long totalBytes);
    private static native boolean nativeDownloadUrlToFileCancelled(long progressToken);
    static native void nativeMusicControlEvent(String eventName);

    public String getInternalFilesDirPath() {
        return getFilesDir().getAbsolutePath();
    }

    public String hasManageExternalStorageBuildVariant() {
        return BuildConfig.ASOBMSHOW_MANAGE_EXTERNAL_STORAGE ? "1" : "0";
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
        if (requestCode == REQUEST_OPEN_ARCHIVE ||
                requestCode == REQUEST_OPEN_IMPORT_FOLDER) {
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
                Uri importUri = data.getData();
                boolean isTree = DocumentsContract.isTreeUri(importUri);
                boolean wantsTree = requestCode == REQUEST_OPEN_IMPORT_FOLDER;
                if (isTree != wantsTree) {
                    archivePickerUri.set(null);
                    archivePickerName.set("");
                    archivePickerTree.set(false);
                    archivePickerError.set(wantsTree
                            ? "Selected item is not a folder."
                            : "Selected item is not an archive file.");
                    finishArchivePicker();
                    return;
                }
                String displayName = isTree ? displayNameForTree(importUri)
                        : displayNameForUri(importUri);
                if (!isTree && !isSupportedArchiveUri(importUri, displayName)) {
                    archivePickerUri.set(null);
                    archivePickerName.set("");
                    archivePickerTree.set(false);
                    archivePickerError.set("Selected file is not a supported archive.");
                    finishArchivePicker();
                    return;
                }
                if (isTree) {
                    int flags = data.getFlags()
                            & (Intent.FLAG_GRANT_READ_URI_PERMISSION
                            | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                    try {
                        getContentResolver().takePersistableUriPermission(
                                importUri, flags & Intent.FLAG_GRANT_READ_URI_PERMISSION);
                    } catch (Exception ignored) {
                        // Some providers grant transient access only; keep using it for this copy.
                    }
                }
                archivePickerUri.set(importUri);
                archivePickerName.set(displayName);
                archivePickerTree.set(isTree);
                archivePickerError.set("");
            } else {
                archivePickerUri.set(null);
                archivePickerName.set("");
                archivePickerTree.set(false);
                archivePickerError.set("");
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
            archivePickerTree.set(false);
            archivePickerError.set("");
            latch = new CountDownLatch(1);
            archivePickerLatch = latch;
        }

        runOnUiThread(() -> {
            Intent archiveIntent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            archiveIntent.addCategory(Intent.CATEGORY_OPENABLE);
            archiveIntent.setType("*/*");
            archiveIntent.putExtra(Intent.EXTRA_MIME_TYPES, archiveMimeTypes());
            archiveIntent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);

            try {
                startActivityForResult(archiveIntent, REQUEST_OPEN_ARCHIVE);
            } catch (Exception e) {
                archivePickerUri.set(null);
                archivePickerName.set("");
                archivePickerTree.set(false);
                archivePickerError.set("Could not open archive picker.");
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
            String pickerError = archivePickerError.get();
            if (!pickerError.isEmpty()) {
                return ERROR_PREFIX + pickerError;
            }
            return ERROR_PREFIX + "Archive selection was cancelled.";
        }
        return startPendingImportCopy(uri, archivePickerName.get(), archivePickerTree.get());
    }

    public String pickFolderForImport() {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return ERROR_PREFIX + "Folder import picker cannot block the UI thread.";
        }

        CountDownLatch latch;
        synchronized (archivePickerLock) {
            if (archivePickerLatch != null) {
                return ERROR_PREFIX + "Import picker is already open.";
            }
            archivePickerUri.set(null);
            archivePickerName.set("");
            archivePickerTree.set(false);
            archivePickerError.set("");
            latch = new CountDownLatch(1);
            archivePickerLatch = latch;
        }

        runOnUiThread(() -> {
            Intent folderIntent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            folderIntent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                    | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
            try {
                startActivityForResult(folderIntent, REQUEST_OPEN_IMPORT_FOLDER);
            } catch (Exception e) {
                archivePickerUri.set(null);
                archivePickerName.set("");
                archivePickerTree.set(false);
                archivePickerError.set("Could not open folder picker.");
                finishArchivePicker();
            }
        });

        try {
            latch.await();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return ERROR_PREFIX + "Folder import picker was interrupted.";
        }

        synchronized (archivePickerLock) {
            archivePickerLatch = null;
        }

        Uri uri = archivePickerUri.get();
        if (uri == null) {
            String pickerError = archivePickerError.get();
            if (!pickerError.isEmpty()) {
                return ERROR_PREFIX + pickerError;
            }
            return ERROR_PREFIX + "Folder selection was cancelled.";
        }
        return startPendingImportCopy(uri, archivePickerName.get(), true);
    }

    public String consumePendingArchiveImport() {
        synchronized (pendingArchiveImportLock) {
            if (!pendingArchiveImportResults.isEmpty()) {
                return pendingArchiveImportResults.removeFirst();
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

    public String downloadUrlText(String urlText) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return ERROR_PREFIX + "Network download cannot block the UI thread.";
        }

        HttpURLConnection connection = null;
        try {
            connection = openHttpConnection(urlText, "GET", 8);

            int statusCode = connection.getResponseCode();
            if (statusCode >= 400) {
                return ERROR_PREFIX + "HTTP " + statusCode + " while downloading " + urlText;
            }

            try (InputStream input = connection.getInputStream()) {
                return readTextResponse(input);
            }
        } catch (Exception e) {
            String message = e.getMessage();
            return ERROR_PREFIX + (message == null || message.isEmpty()
                    ? e.getClass().getSimpleName()
                    : message);
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    public String postUrlText(String urlText) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return ERROR_PREFIX + "Network request cannot block the UI thread.";
        }

        HttpURLConnection connection = null;
        try {
            connection = openHttpConnection(urlText, "POST", 8);

            int statusCode = connection.getResponseCode();
            if (statusCode >= 400) {
                return ERROR_PREFIX + "HTTP " + statusCode + " while posting " + urlText;
            }

            try (InputStream input = connection.getInputStream()) {
                return readTextResponse(input);
            }
        } catch (Exception e) {
            String message = e.getMessage();
            return ERROR_PREFIX + (message == null || message.isEmpty()
                    ? e.getClass().getSimpleName()
                    : message);
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    public String downloadUrlToFile(String urlText, String pathText, long progressToken) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            return ERROR_PREFIX + "Network download cannot block the UI thread.";
        }

        File target = new File(pathText);
        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            return ERROR_PREFIX + "Could not create download directory.";
        }

        HttpURLConnection connection = null;
        try {
            connection = openHttpConnection(urlText, "GET", 8);

            int statusCode = connection.getResponseCode();
            if (statusCode >= 400) {
                return ERROR_PREFIX + "HTTP " + statusCode + " while downloading " + urlText;
            }

            long contentLength = Math.max(0, connection.getContentLengthLong());
            nativeDownloadUrlToFileProgress(progressToken, 0, contentLength);

            try (InputStream input = connection.getInputStream();
                 FileOutputStream output = new FileOutputStream(target, false)) {
                copyBinaryResponse(input, output, progressToken, contentLength);
            } catch (Exception e) {
                target.delete();
                throw e;
            }
            return "OK";
        } catch (Exception e) {
            String message = e.getMessage();
            return ERROR_PREFIX + (message == null || message.isEmpty()
                    ? e.getClass().getSimpleName()
                    : message);
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    public String loadNativeMusic(String pathText, String metadataText, long durationMicros) {
        synchronized (nativeMusicLock) {
            releaseNativeMusicPlayerLocked();
            try {
                MediaPlayer player = new MediaPlayer();
                player.setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build());
                player.setDataSource(pathText);
                player.prepare();
                String[] metadataLines = metadataText == null
                        ? new String[0]
                        : metadataText.split("\\n", -1);
                nativeMusicTitle = metadataLine(metadataLines, 0, new File(pathText).getName());
                nativeMusicArtist = metadataLine(metadataLines, 1, "AsoBMaShow");
                nativeMusicAlbum = metadataLine(metadataLines, 2, "");
                nativeMusicArtworkPath = metadataLine(metadataLines, 3, "");
                nativeMusicArtwork = decodeNativeMusicArtwork(nativeMusicArtworkPath);
                player.setOnCompletionListener(ignored ->
                        nativeMusicControlEvent("finished"));
                nativeMusicPlayer = player;
                nativeMusicDurationMicros = durationMicros > 0
                        ? durationMicros
                        : Math.max(0L, player.getDuration()) * 1000L;
                ensureNativeMusicSessionLocked();
                updateNativeMusicSessionLocked(false, false);
                return "OK";
            } catch (Exception e) {
                releaseNativeMusicPlayerLocked();
                return ERROR_PREFIX + messageForException(e, "Could not load music.");
            }
        }
    }

    public String updateNativeMusicMetadata(String metadataText, long durationMicros) {
        synchronized (nativeMusicLock) {
            if (nativeMusicPlayer == null) {
                return "OK";
            }
            try {
                String[] metadataLines = metadataText == null
                        ? new String[0]
                        : metadataText.split("\\n", -1);
                nativeMusicTitle = metadataLine(metadataLines, 0, "AsoBMaShow");
                nativeMusicArtist = metadataLine(metadataLines, 1, "AsoBMaShow");
                nativeMusicAlbum = metadataLine(metadataLines, 2, "");
                nativeMusicArtworkPath = metadataLine(metadataLines, 3, "");
                nativeMusicArtwork = decodeNativeMusicArtwork(nativeMusicArtworkPath);
                if (durationMicros > 0) {
                    nativeMusicDurationMicros = durationMicros;
                }
                updateNativeMusicSessionLocked(nativeMusicPlayer.isPlaying());
                return "OK";
            } catch (Exception e) {
                return ERROR_PREFIX + messageForException(
                        e, "Could not update music metadata.");
            }
        }
    }

    public String updateNativeMusicQueue(String queueTitle, String queueText, long currentIndex) {
        synchronized (nativeMusicLock) {
            try {
                ArrayList<MediaSession.QueueItem> nextQueue = new ArrayList<>();
                String[] lines = queueText == null || queueText.isEmpty()
                        ? new String[0]
                        : queueText.split("\\n", -1);
                for (String line : lines) {
                    if (line == null || line.trim().isEmpty()) {
                        continue;
                    }
                    String[] fields = line.split("\\t", -1);
                    long itemId = parseLongOrDefault(
                            metadataField(fields, 0, ""),
                            nextQueue.size() + 1L);
                    String title = metadataField(fields, 1, "AsoBMaShow");
                    String artist = metadataField(fields, 2, "AsoBMaShow");
                    String album = metadataField(fields, 3, "");
                    String artworkPath = metadataField(fields, 4, "");
                    MediaDescription.Builder description = new MediaDescription.Builder()
                            .setMediaId(Long.toString(itemId))
                            .setTitle(title)
                            .setSubtitle(artist)
                            .setDescription(album);
                    if (!artworkPath.isEmpty()) {
                        File artworkFile = new File(artworkPath);
                        if (artworkFile.exists()) {
                            description.setIconUri(Uri.fromFile(artworkFile));
                        }
                    }
                    nextQueue.add(new MediaSession.QueueItem(
                            description.build(), itemId));
                }

                nativeMusicQueueTitle =
                        queueTitle == null || queueTitle.trim().isEmpty()
                                ? "Now Playing"
                                : queueTitle.trim();
                nativeMusicQueue.clear();
                nativeMusicQueue.addAll(nextQueue);
                nativeMusicActiveQueueItemId = NATIVE_MUSIC_UNKNOWN_QUEUE_ID;
                if (currentIndex >= 0 && currentIndex < nativeMusicQueue.size()) {
                    nativeMusicActiveQueueItemId =
                            nativeMusicQueue.get((int) currentIndex).getQueueId();
                }
                if (nativeMusicSession != null) {
                    boolean playing = nativeMusicPlayer != null && nativeMusicPlayer.isPlaying();
                    updateNativeMusicSessionLocked(playing);
                }
                return "OK";
            } catch (Exception e) {
                return ERROR_PREFIX + messageForException(
                        e, "Could not update music queue.");
            }
        }
    }

    public String playNativeMusic() {
        synchronized (nativeMusicLock) {
            if (nativeMusicPlayer == null) {
                return ERROR_PREFIX + "No music is loaded.";
            }
            try {
                nativeMusicPlayer.start();
                updateNativeMusicSessionLocked(true);
                return "OK";
            } catch (Exception e) {
                return ERROR_PREFIX + messageForException(e, "Could not play music.");
            }
        }
    }

    public String pauseNativeMusic() {
        synchronized (nativeMusicLock) {
            if (nativeMusicPlayer == null) {
                return ERROR_PREFIX + "No music is loaded.";
            }
            try {
                if (nativeMusicPlayer.isPlaying()) {
                    nativeMusicPlayer.pause();
                }
                updateNativeMusicSessionLocked(false);
                return "OK";
            } catch (Exception e) {
                return ERROR_PREFIX + messageForException(e, "Could not pause music.");
            }
        }
    }

    public String stopNativeMusic() {
        synchronized (nativeMusicLock) {
            if (nativeMusicPlayer == null) {
                return ERROR_PREFIX + "No music is loaded.";
            }
            try {
                if (nativeMusicPlayer.isPlaying()) {
                    nativeMusicPlayer.pause();
                }
                nativeMusicPlayer.seekTo(0, MediaPlayer.SEEK_CLOSEST);
                updateNativeMusicSessionLocked(false, false);
                stopNativeMusicForegroundServiceLocked();
                return "OK";
            } catch (Exception e) {
                return ERROR_PREFIX + messageForException(e, "Could not stop music.");
            }
        }
    }

    public String seekNativeMusic(String positionMicrosText) {
        synchronized (nativeMusicLock) {
            if (nativeMusicPlayer == null) {
                return ERROR_PREFIX + "No music is loaded.";
            }
            try {
                long positionMicros = Math.max(0L, Long.parseLong(positionMicrosText));
                nativeMusicPlayer.seekTo(positionMicros / 1000L, MediaPlayer.SEEK_CLOSEST);
                updateNativeMusicSessionLocked(nativeMusicPlayer.isPlaying());
                return "OK";
            } catch (Exception e) {
                return ERROR_PREFIX + messageForException(e, "Could not seek music.");
            }
        }
    }

    public String nativeMusicState() {
        synchronized (nativeMusicLock) {
            if (nativeMusicPlayer == null) {
                return "0\n0\n0\n0";
            }
            try {
                long positionMicros = Math.max(0L, nativeMusicPlayer.getCurrentPosition()) * 1000L;
                long durationMicros = nativeMusicDurationMicros > 0
                        ? nativeMusicDurationMicros
                        : Math.max(0L, nativeMusicPlayer.getDuration()) * 1000L;
                return "1\n" + (nativeMusicPlayer.isPlaying() ? "1" : "0") +
                        "\n" + positionMicros + "\n" + durationMicros;
            } catch (Exception e) {
                return "0\n0\n0\n0";
            }
        }
    }

    private HttpURLConnection openHttpConnection(String urlText, String method,
                                                 int maxRedirects) throws IOException {
        String currentUrl = urlText;
        String currentMethod = method;
        for (int i = 0; i <= maxRedirects; i++) {
            URL url = new URL(currentUrl);
            HttpURLConnection connection = (HttpURLConnection) url.openConnection();
            connection.setInstanceFollowRedirects(false);
            connection.setRequestMethod(currentMethod);
            connection.setRequestProperty("User-Agent", "AsoBMaShow");
            connection.setConnectTimeout(10_000);
            connection.setReadTimeout(180_000);
            if ("POST".equals(currentMethod)) {
                connection.setDoOutput(true);
                connection.setFixedLengthStreamingMode(0);
                try (OutputStream ignored = connection.getOutputStream()) {
                    // Send an explicit empty POST body.
                }
            }

            int statusCode = connection.getResponseCode();
            if (!isRedirectStatus(statusCode)) {
                return connection;
            }

            String location = connection.getHeaderField("Location");
            connection.disconnect();
            if (location == null || location.isEmpty()) {
                throw new IOException("Redirect did not include a Location header.");
            }
            currentUrl = new URL(url, location).toString();
            if (statusCode != 307 && statusCode != 308) {
                currentMethod = "GET";
            }
        }
        throw new IOException("Too many redirects.");
    }

    private boolean isRedirectStatus(int statusCode) {
        return statusCode == HttpURLConnection.HTTP_MOVED_PERM ||
                statusCode == HttpURLConnection.HTTP_MOVED_TEMP ||
                statusCode == HttpURLConnection.HTTP_SEE_OTHER ||
                statusCode == 307 ||
                statusCode == 308;
    }

    private String readTextResponse(InputStream input) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[16 * 1024];
        int total = 0;
        while (true) {
            int read = input.read(buffer);
            if (read < 0) {
                break;
            }
            total += read;
            if (total > MAX_TEXT_DOWNLOAD_BYTES) {
                throw new IOException("Downloaded text response is too large.");
            }
            output.write(buffer, 0, read);
        }
        return output.toString(StandardCharsets.UTF_8.name());
    }

    private void copyBinaryResponse(InputStream input, FileOutputStream output,
                                    long progressToken, long totalBytes) throws IOException {
        byte[] buffer = new byte[64 * 1024];
        long total = 0;
        long nextProgressAt = 0;
        while (true) {
            if (nativeDownloadUrlToFileCancelled(progressToken)) {
                throw new IOException("Download cancelled.");
            }
            int read = input.read(buffer);
            if (read < 0) {
                break;
            }
            output.write(buffer, 0, read);
            total += read;
            long now = System.nanoTime();
            if (now >= nextProgressAt) {
                nativeDownloadUrlToFileProgress(progressToken, total, totalBytes);
                nextProgressAt = now + 100_000_000L;
            }
        }
        nativeDownloadUrlToFileProgress(progressToken, total, totalBytes > 0 ? totalBytes : total);
    }

    private String metadataLine(String[] lines, int index, String fallback) {
        return metadataField(lines, index, fallback);
    }

    private String metadataField(String[] fields, int index, String fallback) {
        if (fields == null || index < 0 || index >= fields.length) {
            return fallback;
        }
        return fields[index] == null ? "" : fields[index].trim();
    }

    private long parseLongOrDefault(String value, long fallback) {
        try {
            return Long.parseLong(value);
        } catch (Exception ignored) {
            return fallback;
        }
    }

    private void ensureNativeMusicSessionLocked() {
        if (nativeMusicSession != null) {
            return;
        }
        nativeMusicSession = new MediaSession(this, "AsoBMaShowMusic");
        nativeMusicSession.setFlags(MediaSession.FLAG_HANDLES_MEDIA_BUTTONS
                | MediaSession.FLAG_HANDLES_TRANSPORT_CONTROLS);
        nativeMusicSession.setCallback(new MediaSession.Callback() {
            @Override
            public void onPlay() {
                playNativeMusic();
            }

            @Override
            public void onPause() {
                pauseNativeMusic();
            }

            @Override
            public void onStop() {
                stopNativeMusic();
            }

            @Override
            public void onSeekTo(long positionMs) {
                seekNativeMusic(Long.toString(Math.max(0L, positionMs) * 1000L));
            }

            @Override
            public void onSkipToPrevious() {
                nativeMusicControlEvent("previous");
            }

            @Override
            public void onSkipToNext() {
                nativeMusicControlEvent("next");
            }
        });
    }

    private void updateNativeMusicSessionLocked(boolean playing) {
        updateNativeMusicSessionLocked(playing, true);
    }

    private void updateNativeMusicSessionLocked(boolean playing, boolean updateService) {
        ensureNativeMusicSessionLocked();
        long durationMs = Math.max(0L, nativeMusicDurationMicros / 1000L);
        long positionMs = 0L;
        if (nativeMusicPlayer != null) {
            try {
                positionMs = Math.max(0L, nativeMusicPlayer.getCurrentPosition());
            } catch (Exception ignored) {
            }
        }

        MediaMetadata.Builder metadata = new MediaMetadata.Builder()
                .putString(MediaMetadata.METADATA_KEY_TITLE, nativeMusicTitle)
                .putString(MediaMetadata.METADATA_KEY_ARTIST, nativeMusicArtist)
                .putString(MediaMetadata.METADATA_KEY_ALBUM, nativeMusicAlbum)
                .putLong(MediaMetadata.METADATA_KEY_DURATION, durationMs);
        if (nativeMusicArtwork != null) {
            metadata.putBitmap(MediaMetadata.METADATA_KEY_ART, nativeMusicArtwork);
            metadata.putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, nativeMusicArtwork);
        }
        nativeMusicSession.setMetadata(metadata.build());
        if (nativeMusicQueue.isEmpty()) {
            nativeMusicSession.setQueue(null);
            nativeMusicSession.setQueueTitle(null);
        } else {
            nativeMusicSession.setQueue(new ArrayList<>(nativeMusicQueue));
            nativeMusicSession.setQueueTitle(nativeMusicQueueTitle);
        }

        long actions = PlaybackState.ACTION_PLAY
                | PlaybackState.ACTION_PAUSE
                | PlaybackState.ACTION_PLAY_PAUSE
                | PlaybackState.ACTION_STOP
                | PlaybackState.ACTION_SEEK_TO
                | PlaybackState.ACTION_SKIP_TO_PREVIOUS
                | PlaybackState.ACTION_SKIP_TO_NEXT;
        int state = nativeMusicPlayer == null
                ? PlaybackState.STATE_STOPPED
                : (playing ? PlaybackState.STATE_PLAYING : PlaybackState.STATE_PAUSED);
        PlaybackState.Builder playbackState = new PlaybackState.Builder()
                .setActions(actions)
                .setState(state, positionMs, playing ? 1.0f : 0.0f);
        if (nativeMusicActiveQueueItemId != NATIVE_MUSIC_UNKNOWN_QUEUE_ID) {
            playbackState.setActiveQueueItemId(nativeMusicActiveQueueItemId);
        }
        nativeMusicSession.setPlaybackState(playbackState.build());
        nativeMusicSession.setActive(nativeMusicPlayer != null);
        if (updateService) {
            updateNativeMusicForegroundServiceLocked(playing);
        }
    }

    private void updateNativeMusicForegroundServiceLocked(boolean playing) {
        if (nativeMusicPlayer == null || nativeMusicSession == null) {
            stopNativeMusicForegroundServiceLocked();
            return;
        }
        Intent intent = new Intent(this, AsoBMaShowMusicService.class)
                .setAction(AsoBMaShowMusicService.ACTION_UPDATE)
                .putExtra(AsoBMaShowMusicService.EXTRA_TITLE, nativeMusicTitle)
                .putExtra(AsoBMaShowMusicService.EXTRA_ARTIST, nativeMusicArtist)
                .putExtra(AsoBMaShowMusicService.EXTRA_ALBUM, nativeMusicAlbum)
                .putExtra(AsoBMaShowMusicService.EXTRA_ARTWORK_PATH, nativeMusicArtworkPath)
                .putExtra(AsoBMaShowMusicService.EXTRA_PLAYING, playing)
                .putExtra(AsoBMaShowMusicService.EXTRA_SESSION_TOKEN,
                        nativeMusicSession.getSessionToken());
        try {
            if (playing && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                startForegroundService(intent);
            } else {
                startService(intent);
            }
        } catch (Exception ignored) {
        }
    }

    private void stopNativeMusicForegroundServiceLocked() {
        try {
            stopService(new Intent(this, AsoBMaShowMusicService.class));
        } catch (Exception ignored) {
        }
        try {
            NotificationManager manager =
                    (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            if (manager != null) {
                manager.cancel(AsoBMaShowMusicService.NOTIFICATION_ID);
            }
        } catch (Exception ignored) {
        }
    }

    private void releaseNativeMusicPlayerLocked() {
        if (nativeMusicPlayer != null) {
            try {
                nativeMusicPlayer.release();
            } catch (Exception ignored) {
            }
        }
        nativeMusicPlayer = null;
        nativeMusicDurationMicros = 0;
        nativeMusicTitle = "AsoBMaShow";
        nativeMusicArtist = "AsoBMaShow";
        nativeMusicAlbum = "";
        nativeMusicArtworkPath = "";
        nativeMusicArtwork = null;
        nativeMusicQueueTitle = "";
        nativeMusicQueue.clear();
        nativeMusicActiveQueueItemId = NATIVE_MUSIC_UNKNOWN_QUEUE_ID;
        if (nativeMusicSession != null) {
            try {
                updateNativeMusicSessionLocked(false, false);
                nativeMusicSession.setActive(false);
                nativeMusicSession.release();
            } catch (Exception ignored) {
            }
        }
        nativeMusicSession = null;
        stopNativeMusicForegroundServiceLocked();
    }

    private Bitmap decodeNativeMusicArtwork(String pathText) {
        if (pathText == null || pathText.trim().isEmpty()) {
            return null;
        }
        try {
            return BitmapFactory.decodeFile(pathText);
        } catch (Exception ignored) {
            return null;
        }
    }

    private String messageForException(Exception e, String fallback) {
        String message = e.getMessage();
        return message == null || message.isEmpty()
                ? fallback
                : message;
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
                "application/x-lzh",
                "application/x-tar",
                "application/gzip",
                "application/x-gzip",
                "application/x-bzip2",
                "application/x-xz",
                "application/zstd",
                "application/x-zstd"
        };
    }

    private boolean isSupportedArchiveMimeType(String mimeType) {
        if (mimeType == null) {
            return false;
        }
        String lower = mimeType.toLowerCase(Locale.US);
        for (String archiveMimeType : archiveMimeTypes()) {
            if (lower.equals(archiveMimeType)) {
                return true;
            }
        }
        return false;
    }

    private boolean isSupportedArchiveFileName(String displayName) {
        if (displayName == null) {
            return false;
        }
        String lower = displayName.toLowerCase(Locale.US);
        String[] archiveExtensions = new String[] {
                ".tar.bz2", ".tar.gz", ".tar.xz", ".tar.zst", ".tbz2",
                ".tgz", ".txz", ".tzst", ".zip", ".zipx", ".cbz", ".7z",
                ".cb7", ".rar", ".cbr", ".lzh", ".lha", ".tar", ".gz",
                ".bz2", ".xz", ".zst"
        };
        for (String extension : archiveExtensions) {
            if (lower.endsWith(extension)) {
                return true;
            }
        }
        return false;
    }

    private boolean isSupportedArchiveUri(Uri uri, String displayName) {
        if (isSupportedArchiveFileName(displayName)) {
            return true;
        }
        try {
            return isSupportedArchiveMimeType(getContentResolver().getType(uri));
        } catch (Exception ignored) {
            return false;
        }
    }

    private void handleArchiveImportIntent(Intent intent) {
        Uri archiveUri = archiveUriFromIntent(intent);
        if (archiveUri == null) {
            return;
        }
        String displayName = displayNameForUri(archiveUri);
        if (!isSupportedArchiveUri(archiveUri, displayName)) {
            synchronized (pendingArchiveImportLock) {
                pendingArchiveImportResults.addLast(
                        ERROR_PREFIX + "Selected file is not a supported archive.");
            }
            return;
        }
        startPendingImportCopy(archiveUri, displayName, false);
    }

    private String startPendingImportCopy(Uri importUri, String displayName,
                                          boolean isTree) {
        synchronized (pendingArchiveImportLock) {
            pendingArchiveImportRequests.addLast(
                    new PendingImportRequest(importUri, displayName, isTree));
            startNextPendingImportCopyLocked();
        }
        return PENDING_IMPORT_RESULT;
    }

    private void startNextPendingImportCopyLocked() {
        if (pendingArchiveImportCopyRunning ||
                pendingArchiveImportRequests.isEmpty()) {
            return;
        }
        PendingImportRequest request = pendingArchiveImportRequests.removeFirst();
        pendingArchiveImportCopyRunning = true;
        new Thread(() -> {
            String path = "";
            String error = "";
            try {
                path = copyImportUriToInternalStorage(
                        request.uri, request.displayName, request.isTree);
            } catch (Exception e) {
                error = e.getMessage() == null ? "Could not import charts." : e.getMessage();
            }
            synchronized (pendingArchiveImportLock) {
                pendingArchiveImportResults.addLast(
                        error.isEmpty() ? path : ERROR_PREFIX + error);
                pendingArchiveImportCopyRunning = false;
                startNextPendingImportCopyLocked();
            }
        }, request.isTree ? "AsoBMaShowFolderImport"
                : "AsoBMaShowArchiveImport").start();
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

    private String copyImportUriToInternalStorage(Uri uri, String displayName,
                                                  boolean isTree) throws Exception {
        if (isTree) {
            return copyTreeUriToBmsFolder(uri, displayName);
        }
        return copyArchiveUriToInternalStorage(uri, displayName);
    }

    private File documentsBmsDirectory() {
        File base = getExternalFilesDir(null);
        if (base == null) {
            base = getFilesDir();
        }
        return new File(base, "BMS");
    }

    private String copyTreeUriToBmsFolder(Uri treeUri, String displayName) throws Exception {
        File directory = documentsBmsDirectory();
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new Exception("Could not create BMS import folder.");
        }
        File output = uniqueDirectory(directory, sanitizeFileName(displayName));
        if (!output.mkdirs()) {
            throw new Exception("Could not create imported chart copy.");
        }

        try {
            String rootDocumentId = DocumentsContract.getTreeDocumentId(treeUri);
            copyDocumentTreeChildren(treeUri, rootDocumentId, output);
        } catch (Exception e) {
            deleteRecursively(output);
            throw e;
        }
        return output.getAbsolutePath();
    }

    private void copyDocumentTreeChildren(Uri treeUri, String parentDocumentId,
                                          File destination) throws Exception {
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
                throw new Exception("Could not read selected folder.");
            }
            int idColumn = cursor.getColumnIndexOrThrow(Document.COLUMN_DOCUMENT_ID);
            int nameColumn = cursor.getColumnIndexOrThrow(Document.COLUMN_DISPLAY_NAME);
            int mimeColumn = cursor.getColumnIndexOrThrow(Document.COLUMN_MIME_TYPE);
            while (cursor.moveToNext()) {
                String documentId = cursor.getString(idColumn);
                String name = sanitizeFileName(cursor.getString(nameColumn));
                String mimeType = cursor.getString(mimeColumn);
                if (name.isEmpty()) {
                    name = "item";
                }
                if (Document.MIME_TYPE_DIR.equals(mimeType)) {
                    File childDirectory = uniqueDirectory(destination, name);
                    if (!childDirectory.mkdirs()) {
                        throw new Exception("Could not create folder: " + name);
                    }
                    copyDocumentTreeChildren(treeUri, documentId, childDirectory);
                } else {
                    Uri documentUri =
                            DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId);
                    File output = uniqueFile(destination, name);
                    copyDocumentUriToFile(documentUri, output);
                }
            }
        }
    }

    private void copyDocumentUriToFile(Uri uri, File output) throws Exception {
        try (InputStream input = getContentResolver().openInputStream(uri);
             FileOutputStream outputStream = new FileOutputStream(output)) {
            if (input == null) {
                throw new Exception("Could not open imported file.");
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

    private File uniqueDirectory(File directory, String name) {
        File candidate = new File(directory, name);
        if (!candidate.exists()) {
            return candidate;
        }
        for (int i = 2; i < 10000; i++) {
            candidate = new File(directory, name + " " + i);
            if (!candidate.exists()) {
                return candidate;
            }
        }
        return new File(directory, name + " " + System.currentTimeMillis());
    }

    private void deleteRecursively(File file) {
        if (file == null || !file.exists()) {
            return;
        }
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) {
                    deleteRecursively(child);
                }
            }
        }
        file.delete();
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
