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
        public void motionTouch(int device, int pointer, float x, float y, float pressure) {
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

    static final class MotionSink implements AndroidTouchInput.MotionSink {
        long timestamp;
        long observedTimestamp;
        int calls;
        boolean consumed;
        boolean fail;
        View observedView;
        MotionEvent observedEvent;
        public void setTimestamp(long nanos) { timestamp = nanos; }
        public boolean currentMotion(View view, MotionEvent event) {
            ++calls;
            observedTimestamp = timestamp;
            observedView = view;
            observedEvent = event;
            if (fail) throw new IllegalStateException("motion dispatch failed");
            return consumed;
        }
    }

    static void testGenericMotionTimestampScope(int sdk) {
        View view = new View();
        MotionEvent event = new MotionEvent();
        for (int action : new int[] {MotionEvent.ACTION_MOVE, 7, 8}) {
            event.action = action;
            for (boolean consumed : new boolean[] {false, true}) {
                MotionSink sink = new MotionSink();
                sink.consumed = consumed;
                check(AndroidTouchInput.dispatchMotion(view, event, sink) == consumed,
                        "Generic and captured motion preserve SDL consumption");
                check(sink.calls == 1 && sink.observedView == view && sink.observedEvent == event,
                        "Motion delegation preserves the event, source axes, and pointer metadata");
                check(sink.observedTimestamp == (sdk >= 34 ? 100_000_456L : 100_000_000L),
                        "Generic motion scopes the original sample time on every supported API");
                check(sink.timestamp == 0, "Generic motion clears its native timestamp after dispatch");
            }
        }
        MotionSink sink = new MotionSink();
        sink.fail = true;
        try {
            AndroidTouchInput.dispatchMotion(view, event, sink);
            throw new AssertionError("Generic motion exception must propagate");
        } catch (IllegalStateException expected) {
            check(sink.timestamp == 0, "Generic motion clears its timestamp on failed dispatch");
        }
    }

    static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    static void testCompanionPointerCoordinatesBeforeEdges(int sdk) {
        final long timestamp = sdk >= 34 ? 100_000_456L : 100_000_000L;
        for (int action : new int[] {MotionEvent.ACTION_POINTER_DOWN}) {
            for (int edgeIndex : new int[] {0, 1}) {
                MotionEvent event = new MotionEvent();
                event.action = action;
                event.actionIndex = edgeIndex;
                int companion = 1 - edgeIndex;
                int companionId = event.getPointerId(companion);
                for (int tool : new int[] {MotionEvent.TOOL_TYPE_FINGER, MotionEvent.TOOL_TYPE_UNKNOWN,
                        MotionEvent.TOOL_TYPE_MOUSE, MotionEvent.TOOL_TYPE_STYLUS, MotionEvent.TOOL_TYPE_ERASER}) {
                    event.tools[companion] = tool;
                    Sink sink = new Sink();
                    AndroidTouchInput.dispatch(new View(), event, 101, 201, sink);
                    boolean isTouch = tool == MotionEvent.TOOL_TYPE_FINGER || tool == MotionEvent.TOOL_TYPE_UNKNOWN;
                    List<String> expected = isTouch
                            ? List.of(companionId + "@" + timestamp, "current@" + timestamp)
                            : List.of("current@" + timestamp);
                    check(sink.samples.equals(expected),
                            "Held finger moves before indexed edge without inventing a move/down for the edge pointer: " + sink.samples);
                    if (isTouch) {
                        check(Math.abs(sink.firstX - (0.3f + companion * 0.5f)) < 0.0001f
                                && Math.abs(sink.firstY - (0.3f + companion * 0.5f)) < 0.0001f,
                                "Pointer edges retain current companion coordinates, not stale history");
                    }
                    check(sink.currentCalls == 1 && sink.timestamp == 0,
                            "SDL owns the indexed edge once and timestamp state is cleared");
                }
            }
        }
    }

    static void testFinalCoordinatesBeforeRelease(int sdk) {
        final long timestamp = sdk >= 34 ? 100_000_456L : 100_000_000L;
        for (int action : new int[] {MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP}) {
            for (int edgeIndex = 0; edgeIndex < (action == MotionEvent.ACTION_UP ? 1 : 2); ++edgeIndex) {
                for (int tool : new int[] {MotionEvent.TOOL_TYPE_FINGER, MotionEvent.TOOL_TYPE_UNKNOWN,
                        MotionEvent.TOOL_TYPE_MOUSE, MotionEvent.TOOL_TYPE_STYLUS, MotionEvent.TOOL_TYPE_ERASER}) {
                    for (boolean cancelled : new boolean[] {false, true}) {
                        MotionEvent event = new MotionEvent();
                        event.action = action;
                        event.actionIndex = edgeIndex;
                        event.pointerCount = action == MotionEvent.ACTION_UP ? 1 : 2;
                        event.tools[edgeIndex] = tool;
                        event.flags = cancelled ? MotionEvent.FLAG_CANCELED : 0;
                        Sink sink = new Sink();
                        AndroidTouchInput.dispatch(new View(), event, 101, 201, sink);
                        List<String> expected = new ArrayList<>();
                        boolean moveEdge = (tool == MotionEvent.TOOL_TYPE_FINGER || tool == MotionEvent.TOOL_TYPE_UNKNOWN)
                                && !(sdk >= 33 && cancelled);
                        if (edgeIndex != 0 || moveEdge) expected.add("4@" + timestamp);
                        if (event.pointerCount == 2 && (edgeIndex != 1 || moveEdge)) expected.add("9@" + timestamp);
                        expected.add("current@" + timestamp);
                        check(sink.samples.equals(expected),
                                "Release keeps final finger movement before SDL up, except rejected or non-touch indexed pointers: " + sink.samples);
                        if (expected.size() > 1) {
                            float coordinate = expected.get(0).startsWith("4@") ? 0.3f : 0.8f;
                            check(Math.abs(sink.firstX - coordinate) < 0.0001f
                                    && Math.abs(sink.firstY - coordinate) < 0.0001f,
                                    "Release movement uses the final current coordinate, not prior history");
                        }
                        check(sink.currentCalls == 1 && sink.timestamp == 0,
                                "SDL still receives the original release exactly once");
                    }
                }
            }
        }
    }

    public static void main(String[] args) {
        for (int sdk : new int[] {28, 33, 34, 36}) {
            Build.VERSION.SDK_INT = sdk;
            testGenericMotionTimestampScope(sdk);
            testCompanionPointerCoordinatesBeforeEdges(sdk);
            testFinalCoordinatesBeforeRelease(sdk);
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

            for (int action : new int[] {0, 3}) {
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
