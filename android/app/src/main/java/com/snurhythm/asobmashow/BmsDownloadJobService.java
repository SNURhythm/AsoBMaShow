package com.snurhythm.asobmashow;

import android.app.job.*;
import android.os.Build;

public final class BmsDownloadJobService extends JobService {
    @Override public boolean onStartJob(JobParameters parameters) {
        if (!"gingerrush-v1".equals(parameters.getExtras().getString("source_scope"))) return false;
        BmsDownloads.initialize(this);
        String id = parameters.getExtras().getString("id", "");
        long attempt = parameters.getExtras().getLong("attempt", 0);
        if (Build.VERSION.SDK_INT < 34 || !BmsDownloads.nativeCurrent(id, attempt)) return false;
        setNotification(parameters, BmsDownloads.notificationId(id), BmsDownloads.notification(id, attempt, "BMS download", 0, 0), JOB_END_NOTIFICATION_POLICY_REMOVE);
        return BmsDownloads.start(id, attempt, notification -> setNotification(parameters,
                BmsDownloads.notificationId(id), notification, JOB_END_NOTIFICATION_POLICY_REMOVE),
                () -> jobFinished(parameters, false));
    }
    @Override public boolean onStopJob(JobParameters parameters) {
        if (!"gingerrush-v1".equals(parameters.getExtras().getString("source_scope"))) return false;
        BmsDownloads.interrupted(parameters.getExtras().getString("id", ""), parameters.getExtras().getLong("attempt", 0));
        return false;
    }
}
