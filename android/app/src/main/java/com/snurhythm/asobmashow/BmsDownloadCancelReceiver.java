package com.snurhythm.asobmashow;

import android.content.*;

public final class BmsDownloadCancelReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        BmsDownloads.initialize(context);
        String id = intent.getStringExtra("id");
        if (id != null) BmsDownloads.nativeCancel(id, intent.getLongExtra("attempt", 0));
    }
}
