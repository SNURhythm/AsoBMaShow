package com.snurhythm.asobmashow;

import android.os.Build;
import android.view.MotionEvent;
import android.view.View;

/** Forwards native touch samples to SDL before its current-event gesture handling. */
final class AndroidTouchInput {
    interface Sink {
        void setTimestamp(long uptimeNanos);
        void historicalTouch(int device, int pointer, float x, float y, float pressure);
        boolean currentTouch(View view, MotionEvent event);
    }

    private AndroidTouchInput() {}

    static boolean dispatch(View view, MotionEvent event, float width, float height, Sink sink) {
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
            view.requestUnbufferedDispatch(event);
        }
        try {
            if (event.getActionMasked() == MotionEvent.ACTION_MOVE) {
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
                        sink.historicalTouch(event.getDeviceId(), event.getPointerId(pointer),
                                normalize(event.getHistoricalX(pointer, sample), width),
                                normalize(event.getHistoricalY(pointer, sample), height),
                                Math.min(1.0f, event.getHistoricalPressure(pointer, sample)));
                    }
                }
            }
            sink.setTimestamp(Build.VERSION.SDK_INT >= 34
                    ? event.getEventTimeNanos() : event.getEventTime() * 1_000_000L);
            return sink.currentTouch(view, event);
        } finally {
            sink.setTimestamp(0);
        }
    }

    private static float normalize(float position, float extent) {
        return extent <= 1.0f ? 0.5f : position / (extent - 1.0f);
    }
}
