package com.snurhythm.asobmashow;

import android.os.Build;
import android.view.MotionEvent;
import android.view.View;

/** Keeps gameplay finger gestures on native ingress and other gestures on SDL. */
final class AndroidTouchInput {
    interface Sink {
        long acquireRawTouchGesture();
        void rawTouch(long epoch, int pointer, int phase, float x, float y, long uptimeNanos);
        void setTimestamp(long uptimeNanos);
        void motionTouch(int device, int pointer, float x, float y, float pressure);
        boolean currentTouch(View view, MotionEvent event);
    }

    interface MotionSink {
        void setTimestamp(long uptimeNanos);
        boolean currentMotion(View view, MotionEvent event);
    }

    static boolean dispatchMotion(View view, MotionEvent event, MotionSink sink) {
        try {
            sink.setTimestamp(Build.VERSION.SDK_INT >= 34
                    ? event.getEventTimeNanos() : event.getEventTime() * 1_000_000L);
            return sink.currentMotion(view, event);
        } finally {
            sink.setTimestamp(0);
        }
    }

    private long gestureEpoch;
    private long gestureDownTime = -1;
    private int gestureDevice = -1;

    boolean dispatch(View view, MotionEvent event, float width, float height, Sink sink) {
        final int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            view.requestUnbufferedDispatch(event);
            for (int pointer = 0; pointer < event.getPointerCount(); ++pointer) {
                if (isTouch(event, pointer)) {
                    gestureEpoch = sink.acquireRawTouchGesture();
                    gestureDevice = event.getDeviceId();
                    gestureDownTime = event.getDownTime();
                    break;
                }
            }
        }
        final boolean sameGesture = gestureDevice == event.getDeviceId()
                && gestureDownTime == event.getDownTime();
        try {
            if (!sameGesture || gestureEpoch == 0) return dispatchSdl(view, event, width, height, sink);
            // Finish the whole raw chord/history pass first: even the first
            // SDL sample can wait on unrelated event processing or rendering.
            dispatchRaw(event, width, height, sink, gestureEpoch);
            int nonTouchPointers = 0;
            for (int pointer = 0; pointer < event.getPointerCount(); ++pointer) {
                if (!isTouch(event, pointer)) nonTouchPointers |= 1 << event.getPointerId(pointer);
            }
            if (nonTouchPointers != 0) {
                // Rare mixed-tool gestures retain SDL's pen/mouse behavior,
                // without forwarding any raw-owned finger a second time.
                MotionEvent nonTouch = copyNonTouchEvent(event, nonTouchPointers);
                try {
                    dispatchSdl(view, nonTouch, width, height, sink, event);
                } finally {
                    nonTouch.recycle();
                }
            }
            return true;
        } finally {
            // Session teardown, pause and overflow never change the route of
            // an in-flight gesture. A stale epoch is swallowed by native code.
            if (sameGesture && (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL)) {
                gestureEpoch = 0;
                gestureDownTime = -1;
                gestureDevice = -1;
            }
        }
    }

    private static boolean dispatchSdl(View view, MotionEvent event, float width, float height, Sink sink) {
        return dispatchSdl(view, event, width, height, sink, event);
    }

    private static boolean dispatchSdl(View view, MotionEvent event, float width, float height,
                                       Sink sink, MotionEvent timestampSource) {
        final int action = event.getActionMasked();
        try {
            if (action == MotionEvent.ACTION_MOVE) {
                // MotionEvent batches all pointers by sample time. Replaying
                // each pointer's entire history first would reorder chords.
                for (int sample = 0; sample < event.getHistorySize(); ++sample) {
                    sink.setTimestamp(Build.VERSION.SDK_INT >= 34
                            ? event.getHistoricalEventTimeNanos(sample)
                            : event.getHistoricalEventTime(sample) * 1_000_000L);
                    for (int pointer = 0; pointer < event.getPointerCount(); ++pointer) {
                        int tool = event.getToolType(pointer);
                        if (tool != MotionEvent.TOOL_TYPE_FINGER
                                && tool != MotionEvent.TOOL_TYPE_UNKNOWN) continue;
                        sink.motionTouch(event.getDeviceId(), event.getPointerId(pointer),
                                normalize(event.getHistoricalX(pointer, sample), width),
                                normalize(event.getHistoricalY(pointer, sample), height),
                                Math.min(1.0f, event.getHistoricalPressure(pointer, sample)));
                    }
                }
            }
            sink.setTimestamp(Build.VERSION.SDK_INT >= 34
                    ? timestampSource.getEventTimeNanos() : timestampSource.getEventTime() * 1_000_000L);
            if (action == MotionEvent.ACTION_POINTER_DOWN
                    || action == MotionEvent.ACTION_POINTER_UP || action == MotionEvent.ACTION_UP) {
                // SDL forwards only the indexed edge. Other fingers can move
                // in the same sample, and SDL up uses the last motion position.
                // Preserve final motion except for new or rejected contacts.
                int edgePointer = action == MotionEvent.ACTION_UP ? 0 : event.getActionIndex();
                boolean skipEdge = action == MotionEvent.ACTION_POINTER_DOWN
                        || (Build.VERSION.SDK_INT >= 33 && (event.getFlags() & MotionEvent.FLAG_CANCELED) != 0);
                for (int pointer = 0; pointer < event.getPointerCount(); ++pointer) {
                    if (pointer == edgePointer && skipEdge) continue;
                    int tool = event.getToolType(pointer);
                    if (tool != MotionEvent.TOOL_TYPE_FINGER
                            && tool != MotionEvent.TOOL_TYPE_UNKNOWN) continue;
                    sink.motionTouch(event.getDeviceId(), event.getPointerId(pointer),
                            normalize(event.getX(pointer), width),
                            normalize(event.getY(pointer), height),
                            Math.min(1.0f, event.getPressure(pointer)));
                }
            }
            return sink.currentTouch(view, event);
        } finally {
            sink.setTimestamp(0);
        }
    }

    private static MotionEvent copyNonTouchEvent(MotionEvent event, int pointerIds) {
        int count = Integer.bitCount(pointerIds);
        MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[count];
        MotionEvent.PointerCoords[] coordinates = new MotionEvent.PointerCoords[count];
        int copied = 0;
        int edge = -1;
        for (int pointer = 0; pointer < event.getPointerCount(); ++pointer) {
            if ((pointerIds & (1 << event.getPointerId(pointer))) == 0) continue;
            properties[copied] = new MotionEvent.PointerProperties();
            coordinates[copied] = new MotionEvent.PointerCoords();
            event.getPointerProperties(pointer, properties[copied]);
            event.getPointerCoords(pointer, coordinates[copied]);
            if (pointer == event.getActionIndex()) edge = copied;
            ++copied;
        }
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_POINTER_DOWN || action == MotionEvent.ACTION_POINTER_UP) {
            if (edge < 0) action = MotionEvent.ACTION_MOVE;
            else if (count == 1) action = action == MotionEvent.ACTION_POINTER_DOWN
                    ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_UP;
            else action |= edge << MotionEvent.ACTION_POINTER_INDEX_SHIFT;
        }
        // MotionEvent.split is hidden API. The public obtain overload preserves
        // all current pen/mouse axes and metadata; SDL ignores their history.
        return MotionEvent.obtain(event.getDownTime(), event.getEventTime(), action, count,
                properties, coordinates, event.getMetaState(), event.getButtonState(),
                event.getXPrecision(), event.getYPrecision(), event.getDeviceId(),
                event.getEdgeFlags(), event.getSource(), event.getFlags());
    }

    private static void dispatchRaw(MotionEvent event, float width, float height, Sink sink, long epoch) {
        final int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_MOVE) {
            for (int sample = 0; sample < event.getHistorySize(); ++sample) {
                long time = Build.VERSION.SDK_INT >= 34 ? event.getHistoricalEventTimeNanos(sample)
                        : event.getHistoricalEventTime(sample) * 1_000_000L;
                for (int pointer = 0; pointer < event.getPointerCount(); ++pointer) {
                    if (!isTouch(event, pointer)) continue;
                    sink.rawTouch(epoch, event.getPointerId(pointer), MotionEvent.ACTION_MOVE,
                            normalize(event.getHistoricalX(pointer, sample), width),
                            normalize(event.getHistoricalY(pointer, sample), height), time);
                }
            }
        }
        long time = Build.VERSION.SDK_INT >= 34 ? event.getEventTimeNanos()
                : event.getEventTime() * 1_000_000L;
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_MOVE
                || action == MotionEvent.ACTION_CANCEL) {
            for (int pointer = 0; pointer < event.getPointerCount(); ++pointer) {
                currentRaw(event, pointer, action, width, height, time, sink, epoch);
            }
        } else if (action == MotionEvent.ACTION_POINTER_DOWN || action == MotionEvent.ACTION_POINTER_UP
                || action == MotionEvent.ACTION_UP) {
            int edge = action == MotionEvent.ACTION_UP ? 0 : event.getActionIndex();
            boolean down = action == MotionEvent.ACTION_POINTER_DOWN;
            boolean cancelled = !down && Build.VERSION.SDK_INT >= 33
                    && (event.getFlags() & MotionEvent.FLAG_CANCELED) != 0;
            for (int pointer = 0; pointer < event.getPointerCount(); ++pointer) {
                if (pointer == edge && (down || cancelled)) continue;
                currentRaw(event, pointer, MotionEvent.ACTION_MOVE, width, height, time, sink, epoch);
            }
            currentRaw(event, edge, down ? MotionEvent.ACTION_DOWN
                    : cancelled ? MotionEvent.ACTION_CANCEL : MotionEvent.ACTION_UP,
                    width, height, time, sink, epoch);
        }
    }

    private static void currentRaw(MotionEvent event, int pointer, int phase,
                                   float width, float height, long time, Sink sink, long epoch) {
        if (!isTouch(event, pointer)) return;
        sink.rawTouch(epoch, event.getPointerId(pointer), phase,
                normalize(event.getX(pointer), width), normalize(event.getY(pointer), height), time);
    }

    private static boolean isTouch(MotionEvent event, int pointer) {
        int tool = event.getToolType(pointer);
        return tool == MotionEvent.TOOL_TYPE_FINGER || tool == MotionEvent.TOOL_TYPE_UNKNOWN;
    }

    private static float normalize(float position, float extent) {
        return extent <= 1.0f ? 0.5f : position / (extent - 1.0f);
    }
}
