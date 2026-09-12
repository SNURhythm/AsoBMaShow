package com.snurhythm.asobmashow;

import android.app.Application;
import android.system.ErrnoException;
import android.system.Os;

public final class AsoBMaShowApplication extends Application {
    @Override
    public void onCreate() {
        super.onCreate();
        try {
            Os.setenv("SQLITE_TMPDIR", getCacheDir().getAbsolutePath(), true);
        } catch (ErrnoException error) {
            throw new IllegalStateException("Could not configure SQLite private temporary storage", error);
        }
    }
}
