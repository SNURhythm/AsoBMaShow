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
        boolean forbidSdl;
        long availableEpoch;
        int acquisitions;
        public long acquireRawTouchGesture() { ++acquisitions; return availableEpoch; }
        float firstX, firstY;
        float firstRawX, firstRawY;
        boolean sdlStarted;
        final List<String> samples = new ArrayList<>();
        final List<String> rawSamples = new ArrayList<>();
        final List<Long> rawEpochs = new ArrayList<>();
        MotionEvent lastCurrentEvent;
        public void rawTouch(long epoch, int pointer, int phase, float x, float y, long uptimeNanos) {
            rawEpochs.add(epoch);
            check(!sdlStarted, "Every raw sample in a MotionEvent arrives before any SDL forwarding");
            if (rawSamples.isEmpty()) { firstRawX = x; firstRawY = y; }
            rawSamples.add(pointer + "/" + phase + "@" + uptimeNanos);
        }
        public void setTimestamp(long nanos) {
            check(!forbidSdl, "Raw-owned fingers never enter SDL timestamp scopes");
            timestamp = nanos;
        }
        public void motionTouch(int device, int pointer, float x, float y, float pressure) {
            check(!forbidSdl, "Raw-owned fingers never enter SDL dispatch");
            sdlStarted = true;
            check(device == 7, "History keeps the source device");
            if (samples.isEmpty()) { firstX = x; firstY = y; }
            check(pressure == (pointer == 4 ? 0.5f : 1.0f), "Pressure follows SDL clamping");
            samples.add(pointer + "@" + timestamp);
        }
        public boolean currentTouch(View view, MotionEvent event) {
            check(!forbidSdl, "Raw-owned fingers never enter SDL dispatch");
            sdlStarted = true;
            lastCurrentEvent = event;
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

    static boolean deliver(AndroidTouchInput input, View view, MotionEvent event, float width, float height, Sink sink) {
        sink.sdlStarted = false;
        return input.dispatch(view, event, width, height, sink);
    }

    static boolean dispatchSdl(View view, MotionEvent event, float width, float height, Sink sink) {
        boolean consumed = deliver(new AndroidTouchInput(), view, event, width, height, sink);
        check(sink.rawSamples.isEmpty(), "SDL-owned input never also enters raw gameplay");
        return consumed;
    }

    static boolean dispatchOwned(View view, MotionEvent event, float width, float height, Sink sink) {
        AndroidTouchInput input = new AndroidTouchInput();
        MotionEvent start = new MotionEvent();
        start.action = MotionEvent.ACTION_DOWN;
        start.pointerCount = 1;
        start.deviceId = event.deviceId;
        start.downTime = event.downTime;
        Sink initial = new Sink();
        initial.availableEpoch = 42;
        input.dispatch(new View(), start, width, height, initial);
        sink.availableEpoch = 42;
        return deliver(input, view, event, width, height, sink);
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
                    dispatchSdl(new View(), event, 101, 201, sink);
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
                        dispatchSdl(new View(), event, 101, 201, sink);
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

    static void testRawSamplesPrecedeSdlForTheWholeEvent(int sdk) {
        final long current = sdk >= 34 ? 100_000_456L : 100_000_000L;
        final long first = sdk >= 34 ? 90_000_123L : 90_000_000L;
        final long second = sdk >= 34 ? 95_000_123L : 95_000_000L;
        MotionEvent event = new MotionEvent();
        Sink sink = new Sink();
        dispatchOwned(new View(), event, 101, 201, sink);
        check(sink.rawSamples.equals(List.of("4/2@" + first, "9/2@" + first,
                        "4/2@" + second, "9/2@" + second, "4/2@" + current, "9/2@" + current)),
                "Raw history and current chord samples retain sample-major order before SDL: " + sink.rawSamples);
        check(Math.abs(sink.firstRawX - .2f) < .0001f && Math.abs(sink.firstRawY - .2f) < .0001f,
                "Raw samples use the same normalized historical coordinates");

        for (int edge : new int[] {0, 1}) {
            event.actionIndex = edge;
            String indexed = edge == 0 ? "4" : "9";
            String companion = edge == 0 ? "9" : "4";
            event.action = MotionEvent.ACTION_POINTER_DOWN;
            sink = new Sink();
            dispatchOwned(new View(), event, 101, 201, sink);
            check(sink.rawSamples.equals(List.of(companion + "/2@" + current, indexed + "/0@" + current)),
                    "Raw companion movement precedes the indexed Down without inventing a prior indexed Move");
            event.action = MotionEvent.ACTION_POINTER_UP;
            sink = new Sink();
            dispatchOwned(new View(), event, 101, 201, sink);
            check(sink.rawSamples.equals(List.of("4/2@" + current, "9/2@" + current, indexed + "/1@" + current)),
                    "Raw final positions for all fingers precede the indexed Up");
            event.flags = MotionEvent.FLAG_CANCELED;
            sink = new Sink();
            dispatchOwned(new View(), event, 101, 201, sink);
            check(sink.rawSamples.equals(sdk >= 33
                            ? List.of(companion + "/2@" + current, indexed + "/3@" + current)
                            : List.of("4/2@" + current, "9/2@" + current, indexed + "/1@" + current)),
                    "Palm rejection emits raw Cancel for only the indexed contact, with no final rejected movement");
            event.flags = 0;
        }
        event.pointerCount = 1;
        event.action = MotionEvent.ACTION_DOWN;
        sink = new Sink();
        dispatchOwned(new View(), event, 101, 201, sink);
        check(sink.rawSamples.equals(List.of("4/0@" + current)), "Initial Down is forwarded raw once");
        event.action = MotionEvent.ACTION_UP;
        sink = new Sink();
        dispatchOwned(new View(), event, 101, 201, sink);
        check(sink.rawSamples.equals(List.of("4/2@" + current, "4/1@" + current)),
                "Final raw motion precedes the last Up");
        event.pointerCount = 2;
        for (int tool : new int[] {MotionEvent.TOOL_TYPE_FINGER, MotionEvent.TOOL_TYPE_UNKNOWN,
                MotionEvent.TOOL_TYPE_MOUSE, MotionEvent.TOOL_TYPE_STYLUS, MotionEvent.TOOL_TYPE_ERASER}) {
            event.tools[1] = tool;
            boolean touch = tool == MotionEvent.TOOL_TYPE_FINGER || tool == MotionEvent.TOOL_TYPE_UNKNOWN;
            event.action = MotionEvent.ACTION_MOVE;
            sink = new Sink();
            dispatchOwned(new View(), event, 1, 0, sink);
            check(sink.rawSamples.equals(touch
                            ? List.of("4/2@" + first, "9/2@" + first, "4/2@" + second, "9/2@" + second,
                                    "4/2@" + current, "9/2@" + current)
                            : List.of("4/2@" + first, "4/2@" + second, "4/2@" + current)),
                    "Raw history and current movement exclude mouse, stylus, and eraser sources");
            check(sink.firstRawX == .5f && sink.firstRawY == .5f, "Raw normalization tolerates zero-size surfaces");
            event.action = MotionEvent.ACTION_CANCEL;
            sink = new Sink();
            dispatchOwned(new View(), event, 101, 201, sink);
            check(sink.rawSamples.equals(touch
                            ? List.of("4/3@" + current, "9/3@" + current) : List.of("4/3@" + current)),
                    "Whole-gesture raw Cancel terminates every touch contact without movement or history");
        }
    }

    static void testRawOwnedGestureSkipsSdl() {
        MotionEvent event = new MotionEvent();
        event.action = MotionEvent.ACTION_DOWN;
        event.pointerCount = 1;
        Sink sink = new Sink();
        sink.forbidSdl = true;
        check(dispatchOwned(new View(), event, 101, 201, sink),
                "The raw-owned gesture is consumed without SDL");
    }

    static void testGestureOwnershipSurvivesSessionChanges() {
        AndroidTouchInput input = new AndroidTouchInput();
        View view = new View();
        MotionEvent event = new MotionEvent();
        event.action = MotionEvent.ACTION_DOWN;
        event.pointerCount = 1;
        Sink sink = new Sink();
        sink.availableEpoch = 42;
        sink.forbidSdl = true;
        deliver(input, view, event, 101, 201, sink);
        sink.availableEpoch = 0; // paused, overflowed, or destroyed
        event.action = MotionEvent.ACTION_POINTER_DOWN;
        event.pointerCount = 2;
        event.actionIndex = 1;
        deliver(input, view, event, 101, 201, sink);
        sink.availableEpoch = 99; // a replacement gameplay session
        event.action = MotionEvent.ACTION_POINTER_UP;
        deliver(input, view, event, 101, 201, sink);
        event.action = MotionEvent.ACTION_MOVE;
        event.pointerCount = 1;
        deliver(input, view, event, 101, 201, sink);
        event.action = MotionEvent.ACTION_CANCEL;
        deliver(input, view, event, 101, 201, sink);
        check(sink.acquisitions == 1 && sink.rawEpochs.stream().allMatch(epoch -> epoch == 42),
                "Pause, overflow, replacement, and secondary pointer-up retain the original epoch through cancellation");

        event.action = MotionEvent.ACTION_DOWN;
        deliver(input, view, event, 101, 201, sink);
        check(sink.acquisitions == 2 && sink.rawEpochs.get(sink.rawEpochs.size() - 1) == 99,
                "Only a fresh Down may acquire the replacement session");
        event.action = MotionEvent.ACTION_UP;
        deliver(input, view, event, 101, 201, sink);
        int rawBeforeMenu = sink.rawSamples.size();
        sink.availableEpoch = 0;
        sink.forbidSdl = false;
        event.action = MotionEvent.ACTION_DOWN;
        deliver(input, view, event, 101, 201, sink);
        sink.availableEpoch = 123;
        event.action = MotionEvent.ACTION_MOVE;
        deliver(input, view, event, 101, 201, sink);
        event.action = MotionEvent.ACTION_UP;
        deliver(input, view, event, 101, 201, sink);
        check(sink.rawSamples.size() == rawBeforeMenu && sink.acquisitions == 3,
                "An SDL-owned menu gesture never switches to raw when gameplay starts mid-gesture");
        sink.forbidSdl = true;
        event.action = MotionEvent.ACTION_DOWN;
        deliver(input, view, event, 101, 201, sink);
        check(sink.acquisitions == 4 && sink.rawEpochs.get(sink.rawEpochs.size() - 1) == 123,
                "After the terminal Up a new gesture can acquire gameplay");
        sink.availableEpoch = 124;
        deliver(input, view, event, 101, 201, sink);
        check(sink.rawEpochs.get(sink.rawEpochs.size() - 1) == 124,
                "A replacement initial Down re-evaluates ownership if the prior terminal event was lost");
    }

    static void testMixedToolsAndIndependentMousePreserveTouchOwnership() {
        AndroidTouchInput input = new AndroidTouchInput();
        View view = new View();
        MotionEvent touch = new MotionEvent();
        touch.action = MotionEvent.ACTION_DOWN;
        touch.pointerCount = 1;
        Sink sink = new Sink();
        sink.availableEpoch = 42;
        deliver(input, view, touch, 101, 201, sink);
        for (int tool : new int[] {MotionEvent.TOOL_TYPE_MOUSE, MotionEvent.TOOL_TYPE_STYLUS, MotionEvent.TOOL_TYPE_ERASER}) {
            touch.action = MotionEvent.ACTION_POINTER_DOWN;
            touch.actionIndex = 1;
            touch.pointerCount = 2;
            touch.tools[1] = tool;
            int rawBefore = sink.rawSamples.size();
            deliver(input, view, touch, 101, 201, sink);
            MotionEvent split = sink.lastCurrentEvent;
            check(split != touch && split.pointerCount == 1 && split.getPointerId(0) == 9
                            && split.getToolType(0) == tool && split.action == MotionEvent.ACTION_DOWN,
                    "Only the non-touch pointer is forwarded, with split pointer-down remapped to Down");
            check(split.recycled && !touch.recycled && sink.rawSamples.size() == rawBefore + 1,
                    "The raw companion is delivered once and only the temporary split is recycled");
            touch.action = MotionEvent.ACTION_POINTER_UP;
            deliver(input, view, touch, 101, 201, sink);
            check(sink.lastCurrentEvent.action == MotionEvent.ACTION_UP && sink.lastCurrentEvent.recycled,
                    "The non-touch terminal edge is preserved and recycled");
        }
        touch.action = MotionEvent.ACTION_POINTER_UP;
        touch.actionIndex = 0;
        deliver(input, view, touch, 101, 201, sink);
        check(sink.lastCurrentEvent.action == MotionEvent.ACTION_MOVE
                        && sink.lastCurrentEvent.getPointerId(0) == 9
                        && sink.lastCurrentEvent.getX(0) == 80 && sink.lastCurrentEvent.getY(0) == 160
                        && sink.lastCurrentEvent.getPressure(0) == 1.5f,
                "A lifted raw finger leaves only companion pen movement with intact coordinates and pressure");
        check(sink.samples.get(sink.samples.size() - 1).equals("current@" +
                        (Build.VERSION.SDK_INT >= 34 ? 100_000_456L : 100_000_000L)),
                "The public millisecond event copy retains original nanosecond SDL timestamp scope");
        touch.action = MotionEvent.ACTION_MOVE;
        touch.pointerCount = 1;
        MotionEvent mouse = new MotionEvent();
        mouse.action = MotionEvent.ACTION_DOWN;
        mouse.pointerCount = 1;
        mouse.tools[0] = MotionEvent.TOOL_TYPE_MOUSE;
        mouse.deviceId = 8;
        mouse.downTime = 20;
        sink.availableEpoch = 99;
        deliver(input, view, mouse, 101, 201, sink);
        mouse.action = MotionEvent.ACTION_UP;
        deliver(input, view, mouse, 101, 201, sink);
        sink.forbidSdl = true;
        deliver(input, view, touch, 101, 201, sink);
        check(sink.acquisitions == 1 && sink.rawEpochs.stream().allMatch(epoch -> epoch == 42),
                "Interleaved mouse Down/Up cannot acquire or release the touchscreen gesture");

        sink.forbidSdl = false;
        sink.fail = true;
        touch.action = MotionEvent.ACTION_POINTER_DOWN;
        touch.actionIndex = 1;
        touch.pointerCount = 2;
        touch.tools[1] = MotionEvent.TOOL_TYPE_STYLUS;
        try {
            deliver(input, view, touch, 101, 201, sink);
            throw new AssertionError("Mixed SDL failure must propagate");
        } catch (IllegalStateException expected) {
            check(sink.lastCurrentEvent.recycled && !touch.recycled && sink.timestamp == 0,
                    "Mixed-tool failures recycle the split and close the SDL timestamp scope");
        }
    }

    public static void main(String[] args) {
        testRawOwnedGestureSkipsSdl();
        testGestureOwnershipSurvivesSessionChanges();
        testMixedToolsAndIndependentMousePreserveTouchOwnership();
        for (int sdk : new int[] {28, 33, 34, 36}) {
            Build.VERSION.SDK_INT = sdk;
            testGenericMotionTimestampScope(sdk);
            testCompanionPointerCoordinatesBeforeEdges(sdk);
            testFinalCoordinatesBeforeRelease(sdk);
            testRawSamplesPrecedeSdlForTheWholeEvent(sdk);
            View view = new View();
            MotionEvent event = new MotionEvent();
            Sink sink = new Sink();
            check(!dispatchSdl(view, event, 101, 201, sink),
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
            dispatchSdl(view, event, 1, 0, sink);
            check(sink.samples.size() == 3 && sink.firstX == 0.5f && sink.firstY == 0.5f,
                    "Stylus history stays with SDL, and zero-size surfaces do not divide by zero");

            for (int action : new int[] {0, 3}) {
                event.action = action;
                sink = new Sink();
                dispatchSdl(view, event, 101, 201, sink);
                check(sink.samples.size() == 1 && sink.currentCalls == 1,
                        "Down/up/cancel and pointer transitions cannot replay stale movement history");
            }
            check(view.unbufferedRequests == 1,
                    "Every gesture requests unbuffered dispatch at its initial down");
            sink = new Sink();
            sink.fail = true;
            try {
                dispatchSdl(view, event, 101, 201, sink);
                throw new AssertionError("SDL exception must propagate");
            } catch (IllegalStateException expectedFailure) {
                check(sink.timestamp == 0, "Native timestamp scope clears even when SDL dispatch throws");
            }
        }
    }
}
