package com.snurhythm.asobmashow;

import android.app.NotificationManager;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.Context;
import android.os.Build;

/** Upgrade cleanup only. Never recreates a transfer or deletes imported files. */
final class RetiredBmsDownloads {
    private RetiredBmsDownloads() {}

    static void cancel(Context context) {
        JobScheduler scheduler = context.getSystemService(JobScheduler.class);
        if (scheduler != null) {
            for (JobInfo job : scheduler.getAllPendingJobs()) {
                if (job.getService().getPackageName().equals(context.getPackageName()) &&
                        job.getService().getClassName().equals(
                                "com.snurhythm.asobmashow.BmsDownloadJobService") &&
                        !"gingerrush-v1".equals(job.getExtras().getString("source_scope"))) {
                    scheduler.cancel(job.getId());
                }
            }
        }
        NotificationManager notifications = context.getSystemService(NotificationManager.class);
        if (notifications != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            for (android.service.notification.StatusBarNotification notification : notifications.getActiveNotifications()) {
                if ("bms_downloads".equals(notification.getNotification().getChannelId())) {
                    notifications.cancel(notification.getTag(), notification.getId());
                }
            }
            notifications.deleteNotificationChannel("bms_downloads");
        }
    }
}
