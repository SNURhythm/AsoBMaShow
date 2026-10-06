import java.util.ArrayDeque;

public final class AndroidWindowPresentationFixture {
    public static void main(String[] args) {
        for (int sdk : new int[] {28, 30, 36}) {
            Build.VERSION.SDK_INT = sdk;
            Activity activity = new Activity();
            activity.onCreate(new Bundle());
            activity.window.decor.drain();
            requireImmersive(activity, "cold start");
            activity.window.flags = WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN;
            activity.window.decor.visibility = 0;
            activity.window.insets.hidden = 0;
            activity.onWindowFocusChanged(false);
            if (activity.window.decor.visibility != 0) {
                throw new AssertionError("Do not reclaim system bars while unfocused");
            }
            activity.onWindowFocusChanged(true);
            requireImmersive(activity, "focus return");
        }
    }

    static void requireImmersive(Activity activity, String phase) {
        Window window = activity.window;
        int hiddenBars = View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION;
        if ((window.decor.visibility & hiddenBars) != hiddenBars
                || (window.flags & WindowManager.LayoutParams.FLAG_FULLSCREEN) == 0
                || (window.flags & WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN) != 0) {
            throw new AssertionError(phase + " must hide both system bars without waiting for Home/resume");
        }
        if (!Activity.mFullscreenModeActive) {
            throw new AssertionError("SDL must continue maintaining immersive mode");
        }
        if (Build.VERSION.SDK_INT >= 30 && window.insets.hidden != WindowInsets.Type.systemBars()) {
            throw new AssertionError("Modern system bar insets must be hidden");
        }
    }
}

class Activity extends SDLActivity {
    ACTIVITY_METHODS
    void handleArchiveImportIntent(Object intent) {}
}
class SDLActivity {
    protected static boolean mFullscreenModeActive;
    protected static SDLSurface mSurface = new SDLSurface();
    final Window window = new Window();
    protected void onCreate(Bundle state) {
        // SDL initializes with a queued windowed-style command on the UI thread.
        window.decor.post(() -> {
            window.flags = WindowManager.LayoutParams.FLAG_FORCE_NOT_FULLSCREEN;
            window.decor.visibility = 0;
            mFullscreenModeActive = false;
        });
    }
    public void onWindowFocusChanged(boolean focus) {}
    Window getWindow() { return window; }
    Object getIntent() { return null; }
    static Surface getNativeSurface() { return mSurface.getHolder().getSurface(); }
}
class Bundle {}
class Build { static class VERSION { static int SDK_INT; } }
class Window {
    int flags;
    boolean decorFits = true;
    final View decor = new View();
    final WindowInsetsController insets = new WindowInsetsController();
    WindowManager.LayoutParams attributes = new WindowManager.LayoutParams();
    View getDecorView() { return decor; }
    void addFlags(int value) { flags |= value; }
    void clearFlags(int value) { flags &= ~value; }
    WindowInsetsController getInsetsController() { return insets; }
    void setDecorFitsSystemWindows(boolean value) { decorFits = value; }
    WindowManager.LayoutParams getAttributes() { return attributes; }
    void setAttributes(WindowManager.LayoutParams value) { attributes = value; }
}
class View {
    static final int SYSTEM_UI_FLAG_FULLSCREEN = 4;
    static final int SYSTEM_UI_FLAG_HIDE_NAVIGATION = 2;
    static final int SYSTEM_UI_FLAG_IMMERSIVE_STICKY = 4096;
    static final int SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN = 1024;
    static final int SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION = 512;
    static final int SYSTEM_UI_FLAG_LAYOUT_STABLE = 256;
    int visibility;
    final ArrayDeque<Runnable> pending = new ArrayDeque<>();
    void post(Runnable action) { pending.add(action); }
    void drain() { while (!pending.isEmpty()) pending.remove().run(); }
    void setSystemUiVisibility(int value) { visibility = value; }
}
class WindowManager {
    static class LayoutParams {
        static final int FLAG_FULLSCREEN = 1024;
        static final int FLAG_FORCE_NOT_FULLSCREEN = 2048;
        static final int LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES = 1;
        int layoutInDisplayCutoutMode;
    }
}
class WindowInsets {
    static class Type { static int systemBars() { return 7; } }
}
class WindowInsetsController {
    static final int BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE = 2;
    int hidden;
    void setSystemBarsBehavior(int behavior) {}
    void hide(int types) { hidden |= types; }
}
class AndroidDisplayRefreshRate {
    static boolean requestHighestRefreshRate(Window window) { return true; }
    static boolean requestHighestRefreshRate(Window window, Surface surface) { return true; }
}
class Surface {}
class SurfaceHolder {
    interface Callback {
        void surfaceCreated(SurfaceHolder holder);
        void surfaceChanged(SurfaceHolder holder, int format, int width, int height);
        void surfaceDestroyed(SurfaceHolder holder);
    }
    void addCallback(Callback callback) {}
    Surface getSurface() { return new Surface(); }
}
class SDLSurface {
    final SurfaceHolder holder = new SurfaceHolder();
    SurfaceHolder getHolder() { return holder; }
}
