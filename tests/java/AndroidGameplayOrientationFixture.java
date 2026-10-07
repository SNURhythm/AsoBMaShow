import java.util.concurrent.CountDownLatch;

public final class AndroidGameplayOrientationFixture {
    public static void main(String[] args) {
        // Rotation, current configuration, expected fixed Android orientation.
        // First four are naturally portrait phones, last four landscape tablets.
        int[][] cases = {
            {0, 1, 1}, {1, 2, 0}, {2, 1, 9}, {3, 2, 8},
            {0, 2, 0}, {1, 1, 9}, {2, 2, 8}, {3, 1, 1}
        };
        for (int[] test : cases) {
            for (int mode : new int[] {0, 1, 2}) {
                Activity activity = new Activity();
                activity.display.rotation = test[0];
                activity.resources.configuration.orientation = test[1];
                activity.setScreenOrientation(mode, true);
                require(activity.requestedOrientation == test[2],
                        "gameplay must capture a fixed direction on phones and tablets");
                // Home moves the display to its launcher's orientation. LOCKED
                // follows that global rotation on resume; fixed requests do not.
                activity.display.rotation = 0;
                activity.resources.configuration.orientation = 1;
                require(activity.requestedOrientation != ActivityInfo.SCREEN_ORIENTATION_LOCKED,
                        "resume must not inherit the launcher's orientation");
                activity.setScreenOrientation(mode, true);
                require(activity.requests == 1 && activity.requestedOrientation == test[2],
                        "retry/repeated lock must retain the first captured orientation");
                activity.setScreenOrientation(mode, false);
                int expectedSetting = mode == 1 ? 11 : mode == 2 ? 1 : 13;
                require(activity.requestedOrientation == expectedSetting,
                        "unlock must restore the user's auto/landscape/portrait setting");
                activity.setScreenOrientation(mode, true);
                require(activity.requestedOrientation == 1,
                        "a new gameplay session must capture its new orientation");
            }
        }
        System.out.println("Android gameplay orientation lifetime tests passed");
    }
    static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
}
class Activity {
    private boolean gameplayOrientationLocked;
    final Display display = new Display();
    final Resources resources = new Resources();
    int requestedOrientation, requests;
    void runOnUiThread(Runnable action) { action.run(); }
    void setRequestedOrientation(int orientation) {
        requestedOrientation = orientation;
        ++requests;
    }
    WindowManager getWindowManager() { return new WindowManager(display); }
    Resources getResources() { return resources; }
    ACTIVITY_METHODS
}
class ActivityInfo {
    static final int SCREEN_ORIENTATION_LANDSCAPE = 0;
    static final int SCREEN_ORIENTATION_PORTRAIT = 1;
    static final int SCREEN_ORIENTATION_REVERSE_LANDSCAPE = 8;
    static final int SCREEN_ORIENTATION_REVERSE_PORTRAIT = 9;
    static final int SCREEN_ORIENTATION_USER_LANDSCAPE = 11;
    static final int SCREEN_ORIENTATION_FULL_USER = 13;
    static final int SCREEN_ORIENTATION_LOCKED = 14;
}
class Surface {
    static final int ROTATION_0 = 0, ROTATION_90 = 1, ROTATION_180 = 2, ROTATION_270 = 3;
}
class Configuration {
    static final int ORIENTATION_PORTRAIT = 1, ORIENTATION_LANDSCAPE = 2;
    int orientation;
}
class Resources {
    final Configuration configuration = new Configuration();
    Configuration getConfiguration() { return configuration; }
}
class Display {
    int rotation;
    int getRotation() { return rotation; }
}
class WindowManager {
    final Display display;
    WindowManager(Display display) { this.display = display; }
    Display getDefaultDisplay() { return display; }
}
