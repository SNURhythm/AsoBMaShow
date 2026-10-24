package com.snurhythm.asobmashow;

import android.app.Instrumentation;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;
import android.text.Selection;
import android.text.Spannable;
import android.text.TextPaint;
import android.text.style.BackgroundColorSpan;
import android.text.style.CharacterStyle;
import android.widget.ScrollView;
import android.widget.TextView;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.concurrent.Callable;
import java.util.concurrent.atomic.AtomicReference;

// Runs against the actual Android text widget without adding charts to the
// user's library. All fixture data lives in the temporary native overlay.
final class NativeDocumentInstrumentationChecks {
    static String run(Instrumentation instrumentation) throws Exception {
        AsoBMaShowActivity activity = (AsoBMaShowActivity) instrumentation.startActivitySync(
                new Intent(instrumentation.getTargetContext(), AsoBMaShowActivity.class)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        await(instrumentation, () -> activity.nativeDocumentSurface() != null &&
                activity.nativeDocumentSurface().getWidth() > 0);
        String header = "#TITLE 한글😀 source fixture with enough text to wrap on a phone screen\r\n";
        String row = header + "#00111:010000000100\r\n";
        earlyFocus(instrumentation, activity, row);
        StringBuilder source = new StringBuilder();
        ArrayList<Integer> spans = new ArrayList<>();
        while (source.length() < 1024 * 1024) {
            int start = source.length();
            source.append(row);
            spans.add(start); spans.add(start + 6); spans.add(0);
            spans.add(start + 7); spans.add(start + header.length() - 2); spans.add(4);
            spans.add(start + header.length() + 7); spans.add(source.length() - 2); spans.add(6);
        }
        String original = source.toString();
        int[] triples = new int[spans.size()];
        for (int i = 0; i < triples.length; ++i) triples[i] = spans.get(i);
        int[] colors = {0x70d6ffff, 0xc69af5ff, 0xe6bb78ff, 0xffab82ff,
                0x7bda92ff, 0x99a7bbff, 0xb2c6e5ff, 0x373f4dff};
        NativeDocumentView document = new NativeDocumentView(activity, 0);
        long opened = SystemClock.elapsedRealtime();
        try {
            document.open(original.getBytes(StandardCharsets.UTF_8), 0, 0.15f, 1, 0.85f,
                    0xffffffff, 0x181d27ff, 0xff0000ff, 16);
            document.setAppearance(0xffffffff, 0x181d27ff, 0xff0000ff, 16, colors);
            document.setSyntax(triples, original.length());
            await(instrumentation, () -> {
                TextView text = field(document, "text");
                return text != null && text.getLayout() != null && !text.isLayoutRequested() &&
                        (int) field(document, "activeSyntaxSpans") > 0;
            });
            long openMs = SystemClock.elapsedRealtime() - opened;
            ui(instrumentation, () -> {
                ScrollView scroller = field(document, "scroller");
                TextView text = field(document, "text");
                require(scroller.getScrollY() == 0, "Source opened at y=" + scroller.getScrollY());
                require(text.getSelectionStart() == 0 && text.getSelectionEnd() == 0,
                        "Initial native focus moved the caret to the end");
                require(text.getKeyListener() == null && text.isTextSelectable(), "Not read-only/selectable");
                require(colorAt(text, 0) == 0xff70d6ff, "Initial directive not colored");
                Spannable content = (Spannable) text.getText();
                int data = header.length() + 7;
                require(colorAt(text, data) == 0xffb2c6e5 && colorAt(text, data + 2) == 0xff373f4d &&
                        colorAt(text, data + 8) == 0xffb2c6e5, "Empty data is muted without dimming populated cells");
                boolean grouped = false;
                for (CharacterStyle span : content.getSpans(data + 2, data + 3, CharacterStyle.class))
                    grouped |= content.getSpanStart(span) == data + 2 && content.getSpanEnd(span) == data + 8;
                require(grouped, "Consecutive 00 cells did not share one native run");
                Selection.setSelection((Spannable) text.getText(), 7, 11); // Korean plus emoji.
                require(text.onTextContextMenuItem(android.R.id.copy), "Native Copy failed");
                ClipboardManager clipboard = (ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE);
                require(original.substring(7, 11).contentEquals(clipboard.getPrimaryClip().getItemAt(0).getText()),
                        "Native copy changed Unicode");
                Selection.setSelection((Spannable) text.getText(), 7, 11);
                return null;
            });
            int target = row.length() * 9000;
            document.highlight(new int[]{target, target + 6}, true);
            await(instrumentation, () -> {
                ScrollView scroller = field(document, "scroller");
                TextView text = field(document, "text");
                return scroller.getScrollY() == text.getLayout().getLineTop(
                        text.getLayout().getLineForOffset(target)) + text.getTotalPaddingTop() &&
                        colorAt(text, target) == 0xff70d6ff;
            });
            ui(instrumentation, () -> {
                TextView text = field(document, "text");
                Spannable content = (Spannable) text.getText();
                require(text.getSelectionStart() == 7 && text.getSelectionEnd() == 11,
                        "Practice focus replaced native selection");
                require(content.getSpans(target, target + 6, BackgroundColorSpan.class).length == 1,
                        "Practice background missing");
                require(colorAt(text, 0) == text.getCurrentTextColor(), "Distant scroll retained old colors");
                int active = field(document, "activeSyntaxSpans");
                require(active > 0 && active <= 4096, "Unbounded active syntax spans");
                int begin = field(document, "paintedBegin"), end = field(document, "paintedEnd");
                require(end - begin <= 16384, "Unbounded active syntax characters");
                return null;
            });
            ui(instrumentation, () -> {
                ScrollView scroller = field(document, "scroller");
                scroller.scrollTo(0, 1500); // Manual scrolling does not use document.scroll().
                scroller.fling(12000);
                return null;
            });
            document.highlight(new int[]{target, target + 6}, true);
            stableFocus(instrumentation, document, target);
            document.scroll(4);
            await(instrumentation, () -> ((ScrollView) field(document, "scroller")).getScrollY() == 0 &&
                    colorAt(field(document, "text"), 0) == 0xff70d6ff);
            document.setFrame(0, 0.15f, 0.8f, 0.7f);
            colors[0] = 0x174cc9ff;
            document.setAppearance(0x151515ff, 0xeeeeeeff, 0xff0000ff, 16, colors);
            await(instrumentation, () -> colorAt(field(document, "text"), 0) == 0xff174cc9);
            ui(instrumentation, () -> {
                TextView text = field(document, "text");
                require(original.contentEquals(text.getText()), "Styling changed source contents");
                require(text.getSelectionStart() == 7 && text.getSelectionEnd() == 11,
                        "Theme or resize replaced selection");
                return null;
            });
            return "PASS native document: " + original.length() + " UTF-16 units, " +
                    (triples.length / 3) + " cached spans, open/layout/color " + openMs +
                    " ms; early focus, initial position, Unicode copy, manual scroll/fling, distant focus, bounds, theme, resize, selection";
        } finally {
            document.close();
            await(instrumentation, () -> activity.nativeDocumentView == null);
        }
    }

    private static void earlyFocus(Instrumentation instrumentation, AsoBMaShowActivity activity,
                                   String row) throws Exception {
        StringBuilder source = new StringBuilder();
        for (int i = 0; i < 200; ++i) source.append(row);
        int target = row.length() * 100;
        NativeDocumentView document = new NativeDocumentView(activity, 0);
        try {
            document.open(source.toString().getBytes(StandardCharsets.UTF_8), 0, 0.15f, 1, 0.85f,
                    0xffffffff, 0x181d27ff, 0xff0000ff, 16);
            // Queue the action immediately, before the first native layout.
            document.highlight(new int[]{target, target + 6}, true);
            await(instrumentation, () -> {
                TextView text = field(document, "text");
                ScrollView scroller = field(document, "scroller");
                return text != null && text.getLayout() != null && !text.isLayoutRequested() &&
                        scroller.getScrollY() == text.getLayout().getLineTop(
                                text.getLayout().getLineForOffset(target)) + text.getTotalPaddingTop();
            });
            stableFocus(instrumentation, document, target);
        } finally {
            document.close();
            await(instrumentation, () -> activity.nativeDocumentView == null);
        }
    }

    private static void stableFocus(Instrumentation instrumentation, NativeDocumentView document,
                                    int target) throws Exception {
        await(instrumentation, () -> {
            ScrollView scroller = field(document, "scroller");
            TextView text = field(document, "text");
            return scroller.getScrollY() == text.getLayout().getLineTop(
                    text.getLayout().getLineForOffset(target)) + text.getTotalPaddingTop();
        });
        AtomicReference<Throwable> error = new AtomicReference<>();
        java.util.concurrent.atomic.AtomicBoolean done = new java.util.concurrent.atomic.AtomicBoolean();
        ui(instrumentation, () -> {
            ScrollView scroller = field(document, "scroller");
            TextView text = field(document, "text");
            scroller.postOnAnimation(new Runnable() {
                int frames;
                @Override public void run() {
                    try {
                        scroller.computeScroll();
                        int expected = text.getLayout().getLineTop(text.getLayout().getLineForOffset(target)) +
                                text.getTotalPaddingTop();
                        require(scroller.getScrollY() == expected,
                                "Manual scroll/fling displaced highlight: " + scroller.getScrollY() + " != " + expected);
                        if (++frames < 3) scroller.postOnAnimation(this);
                        else done.set(true);
                    } catch (Throwable failure) { error.set(failure); done.set(true); }
                }
            });
            return null;
        });
        await(instrumentation, done::get);
        if (error.get() != null) throw new AssertionError(error.get());
    }

    private static int colorAt(TextView text, int offset) {
        TextPaint paint = new TextPaint();
        paint.setColor(text.getCurrentTextColor());
        for (CharacterStyle span : ((Spannable) text.getText()).getSpans(offset, offset + 1, CharacterStyle.class))
            span.updateDrawState(paint);
        return paint.getColor();
    }

    @SuppressWarnings("unchecked")
    private static <T> T field(Object object, String name) throws Exception {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return (T) field.get(object);
    }

    private static <T> T ui(Instrumentation instrumentation, Callable<T> work) throws Exception {
        AtomicReference<T> value = new AtomicReference<>();
        AtomicReference<Throwable> error = new AtomicReference<>();
        instrumentation.runOnMainSync(() -> {
            try { value.set(work.call()); } catch (Throwable failure) { error.set(failure); }
        });
        if (error.get() != null) throw new AssertionError(error.get());
        return value.get();
    }

    private static void await(Instrumentation instrumentation, Callable<Boolean> condition) throws Exception {
        long deadline = SystemClock.elapsedRealtime() + 15000;
        do {
            instrumentation.waitForIdleSync();
            if (ui(instrumentation, condition)) return;
            SystemClock.sleep(16);
        } while (SystemClock.elapsedRealtime() < deadline);
        throw new AssertionError("Native document did not reach the expected layout/color state");
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
