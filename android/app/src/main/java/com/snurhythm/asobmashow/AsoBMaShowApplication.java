package com.snurhythm.asobmashow;

import android.app.Application;
import android.system.ErrnoException;
import android.system.Os;

public final class AsoBMaShowApplication extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        try {
            String cachePath = getCacheDir().getAbsolutePath();
            // libc++ otherwise falls back to /data/local/tmp, outside app storage.
            Os.setenv("TMPDIR", cachePath, true);
            Os.setenv("SQLITE_TMPDIR", cachePath, true);
        } catch (ErrnoException error) {
            throw new IllegalStateException("Could not configure private temporary storage", error);
        }
    }
}
