"""Exercise Android refresh-rate requests on a lightweight host JVM."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
JAVA_ROOT = ROOT / "android/app/src/main/java/com/snurhythm/asobmashow"

# Only the Android boundary is substituted; selection and request deduplication
# run from the production helper. Window attributes persist as on Android.
STUBS = {
    "Build.java": """
package android.os;
public class Build {
    public static class VERSION { public static int SDK_INT = 28; }
    public static class VERSION_CODES { public static final int R = 30; }
}
""",
    "Surface.java": """
package android.view;
public class Surface {
    public static final int FRAME_RATE_COMPATIBILITY_DEFAULT = 0;
    public boolean valid = true;
    public boolean releaseOnRequest;
    public int requests;
    public float requestedRate;
    public int requestedCompatibility = -1;
    public boolean isValid() { return valid; }
    public void setFrameRate(float rate, int compatibility) {
        if (android.os.Build.VERSION.SDK_INT < 30)
            throw new AssertionError("Surface frame-rate API requires Android 11");
        if (!valid) throw new AssertionError("Invalid surface must be ignored");
        if (releaseOnRequest) throw new IllegalStateException("Surface released");
        ++requests;
        requestedRate = rate;
        requestedCompatibility = compatibility;
    }
}
""",
    "Display.java": """
package android.view;
public class Display {
    public static class Mode {
        private final int id, width, height;
        private final float rate;
        public Mode(int id, int width, int height, float rate) {
            this.id = id; this.width = width; this.height = height; this.rate = rate;
        }
        public int getModeId() { return id; }
        public int getPhysicalWidth() { return width; }
        public int getPhysicalHeight() { return height; }
        public float getRefreshRate() { return rate; }
    }
    public Mode current;
    public Mode[] supported;
    public boolean valid = true;
    public Mode getMode() { return current; }
    public Mode[] getSupportedModes() { return supported; }
    public boolean isValid() { return valid; }
}
""",
    "WindowManager.java": """
package android.view;
public class WindowManager {
    public static class LayoutParams {
        public int preferredDisplayModeId;
        public float preferredRefreshRate;
    }
    public Display display;
    public Display getDefaultDisplay() { return display; }
}
""",
    "Window.java": """
package android.view;
public class Window {
    public final WindowManager manager = new WindowManager();
    public final WindowManager.LayoutParams attributes = new WindowManager.LayoutParams();
    public int writes;
    public WindowManager getWindowManager() { return manager; }
    public WindowManager.LayoutParams getAttributes() { return attributes; }
    public void setAttributes(WindowManager.LayoutParams value) {
        if (value != attributes) throw new AssertionError("Expected existing attributes");
        ++writes;
    }
}
""",
}


class AndroidDisplayRefreshRateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        java_home = os.environ.get("JAVA_HOME")
        if not java_home and Path("/usr/libexec/java_home").is_file():
            java_home = subprocess.check_output(
                ["/usr/libexec/java_home", "-v", "17"], text=True).strip()
        cls.java = str(Path(java_home) / "bin/java") if java_home else "java"
        javac = str(Path(java_home) / "bin/javac") if java_home else "javac"
        cls.output = tempfile.TemporaryDirectory()
        stub_dir = Path(cls.output.name) / "android/view"
        stub_dir.mkdir(parents=True)
        sources = []
        for name, source in STUBS.items():
            path = stub_dir / name
            path.write_text(source)
            sources.append(str(path))
        subprocess.run([
            javac, "-d", cls.output.name, *sources,
            str(JAVA_ROOT / "AndroidDisplayRefreshRate.java"),
            str(ROOT / "tests/java/AndroidDisplayRefreshRateTests.java"),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.output.cleanup()

    def run_scenario(self, scenario):
        subprocess.run([
            self.java, "-cp", self.output.name,
            "com.snurhythm.asobmashow.AndroidDisplayRefreshRateTests", scenario,
        ], check=True, timeout=15)

    def test_highest_rate_preserves_physical_resolution(self):
        self.run_scenario("same-resolution")

    def test_invalid_rates_are_not_requested(self):
        self.run_scenario("invalid-rates")

    def test_repeated_focus_does_not_rewrite_or_force_active_mode(self):
        self.run_scenario("repeat")

    def test_changed_display_reselects_matching_resolution(self):
        self.run_scenario("display-change")

    def test_disconnected_display_is_ignored(self):
        self.run_scenario("unavailable")

    def test_equal_highest_rate_keeps_current_mode(self):
        self.run_scenario("equal-rate")

    def test_modern_surface_receives_selected_rate(self):
        self.run_scenario("surface-modern")

    def test_recreated_surface_receives_request_without_window_rewrite(self):
        self.run_scenario("surface-recreated")

    def test_old_android_uses_window_preference_only(self):
        self.run_scenario("surface-legacy")

    def test_surface_unavailable_or_released_does_not_crash(self):
        self.run_scenario("surface-unavailable")


if __name__ == "__main__":
    unittest.main()
