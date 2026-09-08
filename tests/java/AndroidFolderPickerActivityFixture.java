package com.snurhythm.asobmashow;

import java.util.concurrent.BlockingQueue;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.lang.reflect.Method;

public final class AndroidFolderPickerActivityFixture {
    public static void main(String[] arguments) throws Exception {
        String scenario = arguments[0];
        PickerActivity activity = new PickerActivity();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        String name = scenario.startsWith("folder") ? "pickChartFolder"
                : "ensureManageExternalStorageAccess";
        Method request = null;
        for (Method candidate : PickerActivity.class.getDeclaredMethods()) {
            if (candidate.getName().equals(name)) request = candidate;
        }
        Method selected = request;
        if (scenario.endsWith("late")) activity.onDestroy();
        Thread worker = new Thread(() -> {
            try {
                Object value = selected.getParameterCount() == 0
                        ? selected.invoke(activity) : selected.invoke(activity, "1");
                if (value == null) throw new AssertionError("Missing cancellation result");
            } catch (Throwable error) {
                failure.set(error);
            }
        });
        activity.nativeWorker = worker;
        worker.start();
        try {
            if (!scenario.endsWith("late")) {
                Runnable dispatch = activity.ui.poll(2, TimeUnit.SECONDS);
                if (dispatch == null) throw new AssertionError("Picker was not queued");
                if (scenario.endsWith("wait")) dispatch.run();
                activity.onDestroy();
                if (scenario.endsWith("dispatch")) {
                    dispatch.run();
                    if (activity.launches != 0) {
                        throw new AssertionError("Destroyed Activity dispatched a picker");
                    }
                }
            }
            worker.join(2000);
            if (worker.isAlive()) throw new AssertionError("Late request waited after destruction");
            if (failure.get() != null) throw new AssertionError(failure.get());
            System.out.println("PASS " + scenario);
        } finally {
            worker.interrupt();
            worker.join(2000);
        }
    }
}

class FakeSdlActivity {
    Thread nativeWorker;
    protected void onDestroy() {
        if (nativeWorker == null) return;
        try {
            nativeWorker.join(2000);
        } catch (InterruptedException error) {
            throw new AssertionError(error);
        }
        if (nativeWorker.isAlive()) {
            throw new AssertionError("SDL join blocked by native folder/permission waiter");
        }
    }
}

class PickerActivity extends FakeSdlActivity {
    final BlockingQueue<Runnable> ui = new LinkedBlockingQueue<>();
    int launches;
    static final String ERROR_PREFIX = "__ERROR__:", CANCELLED_RESULT = "__CANCELLED__";
    static final int REQUEST_OPEN_TREE = 1, REQUEST_MANAGE_EXTERNAL_STORAGE = 2;
    final Object pickerLock = new Object(), manageStorageLock = new Object();
    CountDownLatch pickerLatch, manageStorageLatch;
    final AtomicReference<String> pickerResult = new AtomicReference<>("");
    final Object gyroscopeTurntableLock = new Object(), documentHandoffLock = new Object();
    final Object nativeMusicLock = new Object();
    boolean gyroscopeActivityResumed, documentHandoffDestroyed;
    Dummy gyroscopeTurntableManager;
    DocumentHandoffOperation documentHandoffOperation;
    final Dummy DOCUMENT_HANDOFF_TOKENS = new Dummy();
    void runOnUiThread(Runnable action) { ui.add(action); }
    void startActivityForResult(Intent intent, int code) { launches++; }
    String getPackageName() { return "fixture"; }
    boolean hasManageExternalStorageAccess() { return false; }
    void cancelPendingChartImports() {}
    void nativeGyroscopeActivityDestroyed() {}
    void cancelDocumentHandoffLocked(DocumentHandoffOperation operation) {}
    void interruptDocumentHandoffIo(DocumentHandoffOperation operation) {}
    void dismissDocumentHandoffPicker(DocumentHandoffOperation operation) {}
    void stopMidiInput() {}
    void releaseNativeMusicPlayerLocked() {}
    static boolean nativeChartFolderPickerCancelled(String token) { return false; }
    ACTIVITY_FIELDS
    ACTIVITY_METHODS
}

class Dummy {
    void destroy() {}
    void cancel(Object value) {}
}
class DocumentHandoffOperation { Object operationToken; }
class BuildConfig { static final boolean ASOBMSHOW_MANAGE_EXTERNAL_STORAGE = true; }
class Looper {
    static final Object MAIN = new Object();
    static Object myLooper() { return null; }
    static Object getMainLooper() { return MAIN; }
}
class Settings {
    static final String ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION = "app-permission";
    static final String ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION = "permission";
}
class Uri { static Uri parse(String text) { return new Uri(); } }
class Intent {
    static final String ACTION_OPEN_DOCUMENT_TREE = "tree";
    static final int FLAG_GRANT_READ_URI_PERMISSION = 1;
    static final int FLAG_GRANT_PERSISTABLE_URI_PERMISSION = 2;
    static final int FLAG_GRANT_PREFIX_URI_PERMISSION = 4;
    Intent(String action) {}
    void setData(Uri uri) {}
    void addFlags(int flags) {}
}
