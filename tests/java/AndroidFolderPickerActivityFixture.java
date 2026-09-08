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
        if (scenario.startsWith("resume-") || scenario.startsWith("result-")) {
            testPermissionReturn(activity, scenario);
            return;
        }
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

    private static void testPermissionReturn(PickerActivity activity, String scenario)
            throws Exception {
        AtomicReference<String> result = new AtomicReference<>();
        Thread worker = new Thread(() -> result.set(
                activity.ensureManageExternalStorageAccess("1")));
        worker.start();
        try {
            Runnable dispatch = activity.ui.poll(2, TimeUnit.SECONDS);
            if (dispatch == null) throw new AssertionError("Permission was not queued");
            activity.onPause();
            activity.onResume();
            if (!activity.folderPickerRequests.pending(queuedCode(activity))) {
                throw new AssertionError("Unlaunched permission completed on resume");
            }
            dispatch.run();
            activity.onResume();
            if (!activity.folderPickerRequests.pending(activity.lastCode)) {
                throw new AssertionError("Permission completed without leaving Activity");
            }
            activity.onPause();
            activity.permissionGranted = scenario.endsWith("grant");
            if (scenario.startsWith("result-")) {
                activity.onActivityResult(activity.lastCode, 0, null);
            }
            activity.onResume();
            worker.join(2000);
            if (worker.isAlive()) throw new AssertionError("Permission return did not release waiter");
            String expected = scenario.endsWith("grant") ? "1" : "0";
            if (!expected.equals(result.get())) throw new AssertionError("Wrong permission result");
            activity.onResume();
            System.out.println("PASS " + scenario);
        } finally {
            worker.interrupt();
            worker.join(2000);
        }
    }

    private static int queuedCode(PickerActivity activity) throws Exception {
        java.lang.reflect.Field active = NativeFolderPickerRequests.class.getDeclaredField("active");
        active.setAccessible(true);
        return ((NativeFolderPickerRequests.Request) active.get(activity.folderPickerRequests)).code;
    }
}

class FakeSdlActivity {
    Thread nativeWorker;
    protected void onResume() {}
    protected void onPause() {}
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {}
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
    int lastCode;
    boolean permissionGranted;
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
    void startActivityForResult(Intent intent, int code) { launches++; lastCode = code; }
    String getPackageName() { return "fixture"; }
    boolean hasManageExternalStorageAccess() { return permissionGranted; }
    void setRequestedOrientation(int orientation) {}
    void nativeGyroscopeActivityResumed() {}
    void nativeGyroscopeActivityPaused() {}
    final DocumentHandoffRequestCodeAllocator DOCUMENT_HANDOFF_REQUEST_CODES =
            new DocumentHandoffRequestCodeAllocator(0x5300, 0xffff);
    static final int REQUEST_OPEN_ARCHIVE = 3, REQUEST_OPEN_IMPORT_FOLDER = 4;
    final AtomicReference<Uri> archivePickerUri = new AtomicReference<>();
    final AtomicReference<String> archivePickerName = new AtomicReference<>("");
    final AtomicReference<String> archivePickerError = new AtomicReference<>("");
    final AtomicReference<Boolean> archivePickerTree = new AtomicReference<>(false);
    void completeDocumentSelectionLocked(DocumentHandoffOperation operation, Uri uri, String result) {}
    Dummy getContentResolver() { return new Dummy(); }
    String displayNameForTree(Uri uri) { return "folder"; }
    String directPathForTree(Uri uri) { return "/folder"; }
    String displayNameForUri(Uri uri) { return "archive"; }
    boolean isSupportedArchiveUri(Uri uri, String name) { return true; }
    void finishArchivePicker() {}
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
    void setActivityResumed(boolean resumed) {}
    void takePersistableUriPermission(Uri uri, int flags) {}
    void destroy() {}
    void cancel(Object value) {}
}
class DocumentHandoffOperation { Object operationToken; int requestCode; }
class ActivityInfo { static final int SCREEN_ORIENTATION_LANDSCAPE = 0; }
class Activity { static final int RESULT_OK = -1; }
class DocumentsContract { static boolean isTreeUri(Uri uri) { return true; } }
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
    Uri getData() { return null; }
    int getFlags() { return 0; }
    static final String ACTION_OPEN_DOCUMENT_TREE = "tree";
    static final int FLAG_GRANT_READ_URI_PERMISSION = 1;
    static final int FLAG_GRANT_PERSISTABLE_URI_PERMISSION = 2;
    static final int FLAG_GRANT_PREFIX_URI_PERMISSION = 4;
    Intent(String action) {}
    void setData(Uri uri) {}
    void addFlags(int flags) {}
}
