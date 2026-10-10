"""Exercise the app's Android motion adapter at the SDL forwarding boundary."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JAVA_ROOT = ROOT / "android/app/src/main/java/com/snurhythm/asobmashow"

STUBS = {
    "android/os/Build.java": """package android.os;
public class Build {
    public static class VERSION { public static int SDK_INT = 34; }
}
""",
    "android/view/View.java": """package android.view;
public class View {
    public int unbufferedRequests;
    public void requestUnbufferedDispatch(MotionEvent event) { ++unbufferedRequests; }
}
""",
    "android/view/MotionEvent.java": """package android.view;
import android.os.Build;
public class MotionEvent {
    public static final int ACTION_DOWN=0, ACTION_UP=1, ACTION_MOVE=2,
        ACTION_CANCEL=3, ACTION_POINTER_DOWN=5, ACTION_POINTER_UP=6;
    public static final int TOOL_TYPE_UNKNOWN=0, TOOL_TYPE_FINGER=1,
        TOOL_TYPE_STYLUS=2, TOOL_TYPE_MOUSE=3;
    public int action = ACTION_MOVE;
    public int[] tools = {TOOL_TYPE_FINGER, TOOL_TYPE_FINGER};
    public int getDeviceId() { return 7; }
    public int getActionMasked() { return action; }
    public int getPointerCount() { return 2; }
    public int getPointerId(int pointer) { return pointer == 0 ? 4 : 9; }
    public int getToolType(int pointer) { return tools[pointer]; }
    public int getHistorySize() { return 2; }
    public long getHistoricalEventTime(int sample) { return 90 + sample * 5; }
    public long getHistoricalEventTimeNanos(int sample) {
        if (Build.VERSION.SDK_INT < 34) throw new AssertionError("API 34 required");
        return getHistoricalEventTime(sample) * 1_000_000 + 123;
    }
    public long getEventTime() { return 100; }
    public long getEventTimeNanos() {
        if (Build.VERSION.SDK_INT < 34) throw new AssertionError("API 34 required");
        return 100_000_456;
    }
    public float getHistoricalX(int pointer, int sample) { return 20 + pointer * 50 + sample * 10; }
    public float getHistoricalY(int pointer, int sample) { return 40 + pointer * 100 + sample * 20; }
    public float getHistoricalPressure(int pointer, int sample) { return pointer == 0 ? 0.5f : 1.5f; }
}
""",
}


class AndroidTouchInputTests(unittest.TestCase):
    def test_motion_history_timestamps_and_dispatch_lifecycle(self):
        java_home = os.environ.get("JAVA_HOME")
        if not java_home and Path("/usr/libexec/java_home").is_file():
            java_home = subprocess.check_output(
                ["/usr/libexec/java_home", "-v", "17"], text=True).strip()
        java = str(Path(java_home) / "bin/java") if java_home else "java"
        javac = str(Path(java_home) / "bin/javac") if java_home else "javac"
        with tempfile.TemporaryDirectory() as output:
            sources = []
            for name, source in STUBS.items():
                path = Path(output) / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source)
                sources.append(str(path))
            subprocess.run([
                javac, "-d", output, *sources,
                str(JAVA_ROOT / "AndroidTouchInput.java"),
                str(ROOT / "tests/java/AndroidTouchInputTests.java"),
            ], check=True)
            subprocess.run([
                java, "-cp", output,
                "com.snurhythm.asobmashow.AndroidTouchInputTests",
            ], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
