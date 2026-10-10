package com.snurhythm.asobmashow;

import android.os.Build;
import android.view.MotionEvent;
import android.view.View;
import java.util.ArrayList;
import java.util.List;

public final class AndroidTouchInputTests {
    static final class Sink implements AndroidTouchInput.Sink {
        long timestamp;
        int currentCalls;
        boolean fail;
        float firstX, firstY;
        final List<String> samples = new ArrayList<>();
        public void setTimestamp(long nanos) { timestamp = nanos; }
        public void historicalTouch(int device, int pointer, float x, float y, float pressure) {
            check(device == 7, "History keeps the source device");
            if (samples.isEmpty()) { firstX = x; firstY = y; }
            check(pressure == (pointer == 4 ? 0.5f : 1.0f), "Pressure follows SDL clamping");
            samples.add(pointer + "@" + timestamp);
        }
        public boolean currentTouch(View view, MotionEvent event) {
            ++currentCalls;
            samples.add("current@" + timestamp);
            if (fail) throw new IllegalStateException("dispatch failed");
            return false;
        }
    }

    static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        for (int sdk : new int[] {28, 33, 34, 36}) {
            Build.VERSION.SDK_INT = sdk;
            View view = new View();
            MotionEvent event = new MotionEvent();
            Sink sink = new Sink();
            check(!AndroidTouchInput.dispatch(view, event, 101, 201, sink),
                    "SDL's consumption result is preserved");
            List<String> expected = sdk >= 34
                    ? List.of("4@90000123", "9@90000123", "4@95000123", "9@95000123", "current@100000456")
                    : List.of("4@90000000", "9@90000000", "4@95000000", "9@95000000", "current@100000000");
            check(sink.samples.equals(expected), "History is dispatched sample-first before current: " + sink.samples);
            check(Math.abs(sink.firstX - 0.2f) < 0.0001f
                    && Math.abs(sink.firstY - 0.2f) < 0.0001f,
                    "Historical coordinates use SDL's surface normalization");
            check(sink.currentCalls == 1 && sink.timestamp == 0,
                    "Current sample and gesture processing run once, with timestamp cleared afterward");

            event.tools[1] = MotionEvent.TOOL_TYPE_STYLUS;
            sink = new Sink();
            AndroidTouchInput.dispatch(view, event, 1, 0, sink);
            check(sink.samples.size() == 3 && sink.firstX == 0.5f && sink.firstY == 0.5f,
                    "Stylus history stays with SDL, and zero-size surfaces do not divide by zero");

            for (int action : new int[] {0, 1, 3, 5, 6}) {
                event.action = action;
                sink = new Sink();
                AndroidTouchInput.dispatch(view, event, 101, 201, sink);
                check(sink.samples.size() == 1 && sink.currentCalls == 1,
                        "Down/up/cancel and pointer transitions cannot replay stale movement history");
            }
            check(view.unbufferedRequests == 1,
                    "Every gesture requests unbuffered dispatch at its initial down");
            sink = new Sink();
            sink.fail = true;
            try {
                AndroidTouchInput.dispatch(view, event, 101, 201, sink);
                throw new AssertionError("SDL exception must propagate");
            } catch (IllegalStateException expectedFailure) {
                check(sink.timestamp == 0, "Native timestamp scope clears even when SDL dispatch throws");
            }
        }
    }
}
