package com.snurhythm.asobmashow;

import android.app.*;
import android.content.*;
import android.os.*;

public final class BmsDownloadService extends Service {
    private String activeId;
    private long activeAttempt;
    @Override public IBinder onBind(Intent intent) { return null; }
    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        BmsDownloads.initialize(this);
        String id = intent == null ? null : intent.getStringExtra("id");
        long attempt = intent == null ? 0 : intent.getLongExtra("attempt", 0);
        if (id == null) { stopSelf(startId); return START_NOT_STICKY; }
        startForeground(BmsDownloads.notificationId(id), BmsDownloads.notification(id, attempt, "BMS download", 0, 0));
        activeId = id; activeAttempt = attempt;
        BmsDownloads.start(id, attempt, notification ->
                getSystemService(NotificationManager.class).notify(BmsDownloads.notificationId(id), notification), () -> {
            if (id.equals(activeId) && attempt == activeAttempt) {
                activeId = null;
                stopForeground(STOP_FOREGROUND_REMOVE); stopSelf(startId);
            }
        });
        return START_REDELIVER_INTENT;
    }
    @Override public void onDestroy() {
        if (activeId != null) BmsDownloads.interrupted(activeId, activeAttempt);
        super.onDestroy();
    }
    @Override public void onTimeout(int startId, int fgsType) {
        if (activeId != null) BmsDownloads.interrupted(activeId, activeAttempt);
        stopForeground(STOP_FOREGROUND_REMOVE); stopSelf();
    }
}
