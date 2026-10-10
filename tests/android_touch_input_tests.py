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
        TOOL_TYPE_STYLUS=2, TOOL_TYPE_MOUSE=3, TOOL_TYPE_ERASER=4;
    public static final int FLAG_CANCELED = 32;
    public static final int ACTION_POINTER_INDEX_SHIFT = 8;
    public int action = ACTION_MOVE;
    public int actionIndex = 1;
    public int pointerCount = 2;
    public int flags;
    public int deviceId = 7;
    public long downTime = 10;
    public boolean recycled;
    public int[] pointerIds = {4, 9};
    private int[] sourceIndices = {0, 1};
    private float[] xs = {30, 80}, ys = {60, 160}, pressures = {.5f, 1.5f};
    private int historySize = 2;
    private long eventTime = 100, eventTimeSubmillis = 456;
    public int[] tools = {TOOL_TYPE_FINGER, TOOL_TYPE_FINGER};
    public int getDeviceId() { return deviceId; }
    public long getDownTime() { return downTime; }
    public int getActionMasked() { return action & 255; }
    public int getActionIndex() { return actionIndex; }
    public int getPointerCount() { return pointerCount; }
    public int getFlags() { return flags; }
    public int getPointerId(int pointer) { return pointerIds[pointer]; }
    public int getToolType(int pointer) { return tools[pointer]; }
    public int getHistorySize() { return historySize; }
    public long getHistoricalEventTime(int sample) { return 90 + sample * 5; }
    public long getHistoricalEventTimeNanos(int sample) {
        if (Build.VERSION.SDK_INT < 34) throw new AssertionError("API 34 required");
        return getHistoricalEventTime(sample) * 1_000_000 + 123;
    }
    public long getEventTime() { return eventTime; }
    public long getEventTimeNanos() {
        if (Build.VERSION.SDK_INT < 34) throw new AssertionError("API 34 required");
        return eventTime * 1_000_000 + eventTimeSubmillis;
    }
    public float getHistoricalX(int pointer, int sample) { return 20 + sourceIndices[pointer] * 50 + sample * 10; }
    public float getHistoricalY(int pointer, int sample) { return 40 + sourceIndices[pointer] * 100 + sample * 20; }
    public float getHistoricalPressure(int pointer, int sample) { return sourceIndices[pointer] == 0 ? 0.5f : 1.5f; }
    public float getX(int pointer) { return xs[pointer]; }
    public float getY(int pointer) { return ys[pointer]; }
    public float getPressure(int pointer) { return pressures[pointer]; }
    public static class PointerProperties { public int id, toolType; }
    public static class PointerCoords { public float x, y, pressure; }
    public void getPointerProperties(int pointer, PointerProperties value) {
        value.id = pointerIds[pointer]; value.toolType = tools[pointer];
    }
    public void getPointerCoords(int pointer, PointerCoords value) {
        value.x = getX(pointer); value.y = getY(pointer); value.pressure = getPressure(pointer);
    }
    public int getMetaState() { return 17; }
    public int getButtonState() { return 32; }
    public float getXPrecision() { return 1.25f; }
    public float getYPrecision() { return 2.5f; }
    public int getEdgeFlags() { return 4; }
    public int getSource() { return 0x5002; }
    public static MotionEvent obtain(long downTime, long eventTime, int action, int count,
            PointerProperties[] properties, PointerCoords[] coordinates, int meta, int buttons,
            float xPrecision, float yPrecision, int device, int edgeFlags, int source, int flags) {
        if (meta != 17 || buttons != 32 || xPrecision != 1.25f || yPrecision != 2.5f
                || edgeFlags != 4 || source != 0x5002) throw new AssertionError("Metadata lost");
        MotionEvent result = new MotionEvent();
        result.downTime = downTime;
        result.eventTime = eventTime;
        result.eventTimeSubmillis = 0; // public obtain accepts milliseconds
        result.historySize = 0;
        result.action = action & 255;
        result.actionIndex = action >> ACTION_POINTER_INDEX_SHIFT;
        result.pointerCount = count;
        result.deviceId = device;
        result.flags = flags;
        for (int i = 0; i < count; ++i) {
            result.pointerIds[i] = properties[i].id;
            result.tools[i] = properties[i].toolType;
            result.xs[i] = coordinates[i].x;
            result.ys[i] = coordinates[i].y;
            result.pressures[i] = coordinates[i].pressure;
        }
        return result;
    }
    public void recycle() { recycled = true; }
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
