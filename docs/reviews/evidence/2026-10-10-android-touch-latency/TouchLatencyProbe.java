import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;
import java.lang.reflect.Method;

// Run as adb shell, which already has Android's input-injection permission.
// Realistic holds avoid cmd input tap's identical down/up timestamps.
public final class TouchLatencyProbe {
    private static Object manager;
    private static Method inject;

    private static void send(long down, int action, float[] xs, float y, int count) throws Exception {
        MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[count];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[count];
        for (int i = 0; i < count; ++i) {
            properties[i] = new MotionEvent.PointerProperties();
            properties[i].id = i;
            properties[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords();
            coords[i].x = xs[i]; coords[i].y = y;
            coords[i].pressure = 1; coords[i].size = 1;
        }
        int phase = action & MotionEvent.ACTION_MASK;
        if (phase == MotionEvent.ACTION_UP) coords[0].pressure = 0;
        if (phase == MotionEvent.ACTION_POINTER_UP) coords[count - 1].pressure = 0;
        MotionEvent event = MotionEvent.obtain(down, SystemClock.uptimeMillis(), action,
                count, properties, coords, 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        try {
            if (!((Boolean) inject.invoke(manager, event, 2)))
                throw new IllegalStateException("Input injection was rejected");
        } finally { event.recycle(); }
    }

    public static void main(String[] args) throws Exception {
        if (args.length < 4) throw new IllegalArgumentException("cycles holdMs gapMs y x...");
        int cycles = Integer.parseInt(args[0]);
        int hold = Integer.parseInt(args[1]);
        int gap = Integer.parseInt(args[2]);
        float y = Float.parseFloat(args[3]);
        float[] xs = new float[args.length - 4];
        for (int i = 0; i < xs.length; ++i) xs[i] = Float.parseFloat(args[4 + i]);
        if (xs.length < 1 || xs.length > 5 || cycles < 1 || cycles > 1000 || hold < 1 || gap < 1)
            throw new IllegalArgumentException("Out of bounds workload");
        Class<?> type = Class.forName("android.hardware.input.InputManager");
        manager = type.getMethod("getInstance").invoke(null);
        inject = type.getMethod("injectInputEvent", InputEvent.class, int.class);
        long started = SystemClock.uptimeMillis();
        for (int cycle = 0; cycle < cycles; ++cycle) {
            long down = SystemClock.uptimeMillis();
            send(down, MotionEvent.ACTION_DOWN, xs, y, 1);
            for (int count = 2; count <= xs.length; ++count)
                send(down, MotionEvent.ACTION_POINTER_DOWN | ((count - 1) << MotionEvent.ACTION_POINTER_INDEX_SHIFT), xs, y, count);
            SystemClock.sleep(hold);
            for (int count = xs.length; count > 1; --count)
                send(down, MotionEvent.ACTION_POINTER_UP | ((count - 1) << MotionEvent.ACTION_POINTER_INDEX_SHIFT), xs, y, count);
            send(down, MotionEvent.ACTION_UP, xs, y, 1);
            SystemClock.sleep(gap);
            if ((cycle + 1) % 50 == 0) System.out.println("Completed " + (cycle + 1) + " gestures");
        }
        System.out.println("PASS gestures=" + cycles + " contacts=" + xs.length
                + " elapsedMs=" + (SystemClock.uptimeMillis() - started));
    }
}
