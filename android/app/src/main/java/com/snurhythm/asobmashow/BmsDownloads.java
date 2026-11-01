package com.snurhythm.asobmashow;

import android.app.*;
import android.app.job.*;
import android.content.*;
import android.os.*;
import org.json.*;
import java.io.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicBoolean;

final class BmsDownloads {
    static final String CHANNEL = "bms_downloads";
    private static Context context;
    private static final Map<String, AtomicBoolean> running = new ConcurrentHashMap<>();
    private static final Handler main = new Handler(Looper.getMainLooper());
    static synchronized void initialize(Context value) {
        if (context != null) return;
        System.loadLibrary("SDL3"); System.loadLibrary("SDL3_ttf"); System.loadLibrary("main");
        Context app = value.getApplicationContext();
        File root = new File(app.getNoBackupFilesDir(), "Downloads");
        if (!root.isDirectory() && !root.mkdirs()) throw new IllegalStateException("Private download storage unavailable.");
        nativeInitialize(root.getAbsolutePath());
        context = app;
        NotificationManager notifications = app.getSystemService(NotificationManager.class);
        notifications.createNotificationChannel(new NotificationChannel(CHANNEL, "BMS downloads", NotificationManager.IMPORTANCE_LOW));
    }
    static int notificationId(String id) { return 0x40000000 | (id.hashCode() & 0x3fffffff); }
    static Notification notification(String id, long attempt, String title, long bytes, long total) {
        Intent open = new Intent(context, AsoBMaShowActivity.class).putExtra("bmsDownloads", true);
        PendingIntent content = PendingIntent.getActivity(context, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Intent cancel = new Intent(context, BmsDownloadCancelReceiver.class).putExtra("id", id).putExtra("attempt", attempt);
        PendingIntent action = PendingIntent.getBroadcast(context, notificationId(id), cancel,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        String progress = total > 0 ? (bytes * 100 / total) + "%" : "Downloading…";
        return new Notification.Builder(context, CHANNEL).setSmallIcon(android.R.drawable.stat_sys_download)
                .setContentTitle(title.isEmpty() ? "BMS download" : title).setContentText(progress)
                .setContentIntent(content).setOngoing(true).setOnlyAlertOnce(true)
                .setProgress(1000, total > 0 ? (int)(1000.0 * bytes / total) : 0, total <= 0)
                .addAction(new Notification.Action.Builder(null, "Cancel", action).build()).build();
    }
    // Called from the native application owner. Extras contain identity only.
    static String schedule(String id, long attempt) {
        try {
            if (context == null) return "Download service is not initialized.";
            if (Build.VERSION.SDK_INT >= 34) {
                JobScheduler scheduler = context.getSystemService(JobScheduler.class);
                JobInfo old = scheduler.getPendingJob(notificationId(id));
                if (old != null && !id.equals(old.getExtras().getString("id"))) return "Download scheduling conflict.";
                PersistableBundle extras = new PersistableBundle();
                extras.putString("id", id); extras.putLong("attempt", attempt);
                JobInfo info = new JobInfo.Builder(notificationId(id), new ComponentName(context, BmsDownloadJobService.class))
                        .setUserInitiated(true).setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).setExtras(extras).build();
                if (scheduler.schedule(info) != JobScheduler.RESULT_SUCCESS) return "Open Downloads and retry while the app is visible.";
            } else {
                context.startForegroundService(new Intent(context, BmsDownloadService.class)
                        .putExtra("id", id).putExtra("attempt", attempt));
            }
            return "";
        } catch (RuntimeException error) { return "The system could not start this download. Retry while the app is visible."; }
    }
    static void cancelTransfer(String id) {
        running.forEach((key, cancellation) -> { if (key.startsWith(id + ":")) cancellation.set(true); });
        if (Build.VERSION.SDK_INT >= 34) context.getSystemService(JobScheduler.class).cancel(notificationId(id));
    }
    static boolean recover(String id, long attempt) {
        try {
            String raw = nativeRequest(id, attempt);
            if (raw.isEmpty()) return false;
            JSONObject request = new JSONObject(raw);
            File part = new File(request.getString("part"));
            if (!BmsDownloadTransport.completed(request.getString("url"), part)) return false;
            nativeComplete(id, attempt, part.getAbsolutePath());
            return !nativeCurrent(id, attempt);
        } catch (Exception error) { return false; }
    }
    static void reconcile() {
        // Called once when SDL starts. Restored system jobs can bootstrap without SDL.
        JobScheduler scheduler = context.getSystemService(JobScheduler.class);
        nativeReconcile(scheduler.getAllPendingJobs().stream()
                .filter(job -> job.getService().getClassName().equals(BmsDownloadJobService.class.getName()))
                .map(job -> job.getExtras().getString("id", "")).toArray(String[]::new), running.keySet().stream().map(key -> key.substring(0, key.indexOf(':'))).toArray(String[]::new));
    }
    interface Progress { void update(Notification notification); }
    static boolean start(String id, long attempt, Progress progress, Runnable finished) {
        String transferKey = id + ":" + attempt;
        AtomicBoolean cancellation = new AtomicBoolean();
        if (running.putIfAbsent(transferKey, cancellation) != null) return false;
        new Thread(() -> {
            try {
                String raw = nativeRequest(id, attempt);
                if (raw.isEmpty()) return;
                JSONObject request = new JSONObject(raw);
                String title = request.getString("title");
                File part = new File(request.getString("part"));
                Map<String, String> headers = new HashMap<>();
                JSONArray entries = request.getJSONArray("headers");
                for (int i = 0; i < entries.length(); ++i) {
                    JSONArray entry = entries.getJSONArray(i); headers.put(entry.getString(0), entry.getString(1));
                }
                BmsDownloadTransport.download(request.getString("url"), headers, part, new BmsDownloadTransport.Listener() {
                    long lastProgress;
                    public boolean cancelled() { return cancellation.get() || !nativeCurrent(id, attempt); }
                    public void progress(long bytes, long total) {
                        if (SystemClock.elapsedRealtime() - lastProgress < 500 && bytes != total) return;
                        lastProgress = SystemClock.elapsedRealtime();
                        nativeProgress(id, attempt, bytes, total);
                        Notification update = notification(id, attempt, title, bytes, total);
                        main.post(() -> { if (!cancellation.get() && nativeCurrent(id, attempt)) progress.update(update); });
                    }
                });
                if (!cancellation.get()) nativeComplete(id, attempt, part.getAbsolutePath());
            } catch (Exception error) {
                // Transport errors are authored locally; no URL/cookie/remote body in logs or UI.
                nativeFailed(id, attempt, "Download interrupted or unavailable. Open Downloads to retry.",
                        error instanceof BmsDownloadTransport.HttpFailure ? ((BmsDownloadTransport.HttpFailure)error).status : 0);
            } finally {
                running.remove(transferKey, cancellation);
                main.post(finished);
            }
        }, "BMS transfer").start();
        return true;
    }
    static void interrupted(String id, long attempt) {
        AtomicBoolean cancellation = running.get(id + ":" + attempt);
        if (cancellation != null) cancellation.set(true);
        nativeFailed(id, attempt, "The system stopped the download. Open Downloads to continue.", 0);
    }
    static void acceptTap(Intent intent) {
        if (intent != null && intent.getBooleanExtra("bmsDownloads", false)) {
            nativeShowDownloads(); intent.removeExtra("bmsDownloads");
        }
    }
    private static native void nativeShowDownloads();
    static native void nativeInitialize(String root);
    private static native void nativeReconcile(String[] scheduled, String[] active);
    static native String nativeRequest(String id, long attempt);
    static native boolean nativeCurrent(String id, long attempt);
    private static native void nativeProgress(String id, long attempt, long bytes, long total);
    private static native void nativeComplete(String id, long attempt, String path);
    static native void nativeFailed(String id, long attempt, String message, int httpStatus);
    static native void nativeCancel(String id, long attempt);
}
