package com.snurhythm.asobmashow;

import android.app.Application;
import android.system.Os;

public final class AndroidTemporaryStorageFixture {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void startNativeActivity() {
        String cache = Application.CACHE.getAbsolutePath();
        require(cache.equals(Os.getenv("TMPDIR")),
                "Native startup must inherit private TMPDIR from Application.onCreate");
        require(cache.equals(Os.getenv("SQLITE_TMPDIR")),
                "SQLite and native temporary storage must use the same private cache");
    }

    public static void main(String[] arguments) {
        String scenario = arguments[0];
        if ("fresh".equals(scenario) || "inherited".equals(scenario)
                || "documents-failure".equals(scenario)) {
            if ("documents-failure".equals(scenario)) {
                AsoBMaShowDocumentsProvider.failure = new java.io.IOException("documents unavailable");
            }
            if ("inherited".equals(scenario)) {
                Os.environment.put("TMPDIR", "/data/local/tmp");
                Os.environment.put("SQLITE_TMPDIR", "/unavailable/sqlite-temp");
            }
            new AsoBMaShowApplication().onCreate();
            startNativeActivity();
            require(AsoBMaShowDocumentsProvider.calls == 1
                    && AsoBMaShowDocumentsProvider.privateEnvironmentReady,
                    "Documents initialization must follow private temporary-storage setup");
            require(android.util.Log.lastError == AsoBMaShowDocumentsProvider.failure,
                    "Documents initialization failure must be logged without breaking temporary storage");
            return;
        }

        require("TMPDIR".equals(scenario) || "SQLITE_TMPDIR".equals(scenario),
                "Unknown test scenario");
        Os.failVariable = scenario;
        try {
            new AsoBMaShowApplication().onCreate();
        } catch (IllegalStateException expected) {
            require(AsoBMaShowDocumentsProvider.calls == 0,
                    "Failed environment setup must stop before Documents initialization");
            require(expected.getCause() == Os.failure,
                    "Startup failure must retain the actual setenv failure");
            return;
        }
        throw new AssertionError("Application startup accepted failed setenv for " + scenario);
    }
}
