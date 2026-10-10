package com.snurhythm.asobmashow;

import android.os.Build;
import android.view.MotionEvent;
import android.view.View;

/** Delivers gameplay samples before forwarding the event to SDL's UI path. */
final class AndroidTouchInput {
    interface Sink {
        void rawTouch(int pointer, int phase, float x, float y, long uptimeNanos);
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

    private AndroidTouchInput() {}

    static boolean dispatch(View view, MotionEvent event, float width, float height, Sink sink) {
        final int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            view.requestUnbufferedDispatch(event);
        }
        try {
            // Finish the whole raw chord/history pass first: even the first
            // SDL sample can wait on unrelated event processing or rendering.
            dispatchRaw(event, width, height, sink);
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
                    ? event.getEventTimeNanos() : event.getEventTime() * 1_000_000L);
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

    private static void dispatchRaw(MotionEvent event, float width, float height, Sink sink) {
        final int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_MOVE) {
            for (int sample = 0; sample < event.getHistorySize(); ++sample) {
                long time = Build.VERSION.SDK_INT >= 34 ? event.getHistoricalEventTimeNanos(sample)
                        : event.getHistoricalEventTime(sample) * 1_000_000L;
                for (int pointer = 0; pointer < event.getPointerCount(); ++pointer) {
                    if (!isTouch(event, pointer)) continue;
                    sink.rawTouch(event.getPointerId(pointer), MotionEvent.ACTION_MOVE,
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
                currentRaw(event, pointer, action, width, height, time, sink);
            }
        } else if (action == MotionEvent.ACTION_POINTER_DOWN || action == MotionEvent.ACTION_POINTER_UP
                || action == MotionEvent.ACTION_UP) {
            int edge = action == MotionEvent.ACTION_UP ? 0 : event.getActionIndex();
            boolean down = action == MotionEvent.ACTION_POINTER_DOWN;
            boolean cancelled = !down && Build.VERSION.SDK_INT >= 33
                    && (event.getFlags() & MotionEvent.FLAG_CANCELED) != 0;
            for (int pointer = 0; pointer < event.getPointerCount(); ++pointer) {
                if (pointer == edge && (down || cancelled)) continue;
                currentRaw(event, pointer, MotionEvent.ACTION_MOVE, width, height, time, sink);
            }
            currentRaw(event, edge, down ? MotionEvent.ACTION_DOWN
                    : cancelled ? MotionEvent.ACTION_CANCEL : MotionEvent.ACTION_UP,
                    width, height, time, sink);
        }
    }

    private static void currentRaw(MotionEvent event, int pointer, int phase,
                                   float width, float height, long time, Sink sink) {
        if (!isTouch(event, pointer)) return;
        sink.rawTouch(event.getPointerId(pointer), phase,
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
