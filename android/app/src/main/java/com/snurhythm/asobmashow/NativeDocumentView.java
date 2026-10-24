package com.snurhythm.asobmashow;

import android.graphics.Typeface;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.text.Layout;
import android.text.Selection;
import android.text.Spannable;
import android.text.Spanned;
import android.text.style.BackgroundColorSpan;
import android.util.Log;
import android.util.TypedValue;
import android.view.ActionMode;
import android.view.Gravity;
import android.view.Menu;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.widget.FrameLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicBoolean;

// JNI owns this lifetime gate. Only closed and the Handler cross threads; all
// Android Views, selection state and layout work belong to the main looper.
final class NativeDocumentView {
    private final AsoBMaShowActivity activity;
    private final long id;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final AtomicBoolean closed = new AtomicBoolean();
    private View surface;
    private ViewGroup parent;
    private ScrollView scroller;
    private TextView text;
    private ActionMode selectionMode;
    private Object backCallback;
    private float x, y, width, height;
    private int highlightColor;
    private int[] ranges = new int[0];
    private int focusOffset = -1;
    private final ViewTreeObserver.OnGlobalLayoutListener layoutListener = this::layout;

    private static native void requestDismiss(long id);

    NativeDocumentView(AsoBMaShowActivity activity, long id) {
        this.activity = activity;
        this.id = id;
    }

    private void post(Runnable work) {
        if (closed.get()) return;
        handler.postAtTime(() -> {
            if (closed.get()) return;
            if (activity.nativeDocumentDestroyed || activity.isDestroyed() || activity.isFinishing()) {
                requestDismiss(id);
                close();
                return;
            }
            try {
                work.run();
            } catch (RuntimeException error) {
                Log.w("NativeDocument", "Native text surface failed", error);
                requestDismiss(id);
                close();
            }
        }, this, SystemClock.uptimeMillis());
    }

    void open(byte[] utf8, float x, float y, float width, float height,
              int foreground, int background, int highlight, float fontSize) {
        // Decode real UTF-8, retaining supplementary characters and embedded NUL.
        final String source = new String(utf8, StandardCharsets.UTF_8);
        post(() -> {
            if (activity.nativeDocumentView != null) activity.nativeDocumentView.close();
            activity.nativeDocumentView = this;
            surface = activity.nativeDocumentSurface();
            parent = activity.findViewById(android.R.id.content);
            if (surface == null || parent == null) throw new IllegalStateException("No SDL surface");
            scroller = new ScrollView(activity);
            scroller.setFillViewport(true);
            scroller.setClipToPadding(true);
            text = new TextView(activity);
            text.setTypeface(Typeface.MONOSPACE);
            text.setTextIsSelectable(true);
            text.setKeyListener(null);
            text.setText(source, TextView.BufferType.SPANNABLE);
            text.setGravity(Gravity.TOP | Gravity.START);
            int padding = Math.round(8 * activity.getResources().getDisplayMetrics().density);
            text.setPadding(padding, padding, padding, padding);
            text.setCustomSelectionActionModeCallback(new ActionMode.Callback() {
                @Override public boolean onCreateActionMode(ActionMode mode, Menu menu) {
                    selectionMode = mode;
                    return true; // Preserve Android's standard copy/select-all/context actions.
                }
                @Override public boolean onPrepareActionMode(ActionMode mode, Menu menu) { return false; }
                @Override public boolean onActionItemClicked(ActionMode mode, MenuItem item) { return false; }
                @Override public void onDestroyActionMode(ActionMode mode) {
                    if (selectionMode == mode) selectionMode = null;
                }
            });
            scroller.addView(text, new ScrollView.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            parent.addView(scroller, new FrameLayout.LayoutParams(0, 0, Gravity.TOP | Gravity.LEFT));
            this.x = x; this.y = y; this.width = width; this.height = height;
            appearance(foreground, background, highlight, fontSize);
            parent.getViewTreeObserver().addOnGlobalLayoutListener(layoutListener);
            layout();
            text.requestFocus();
            if (Build.VERSION.SDK_INT >= 33) backCallback = BackApi33.register(activity, this::handleBack);
            // close() can run on the application worker while this UI task runs.
            if (closed.get()) remove();
        });
    }

    public void setFrame(float x, float y, float width, float height) {
        post(() -> {
            this.x = x; this.y = y; this.width = width; this.height = height;
            layout();
        });
    }

    private static float unit(float value) {
        return Float.isFinite(value) ? Math.max(0, Math.min(1, value)) : 0;
    }

    private void layout() {
        if (closed.get() || scroller == null || surface == null || parent == null) return;
        int[] surfacePosition = new int[2];
        int[] parentPosition = new int[2];
        surface.getLocationInWindow(surfacePosition);
        parent.getLocationInWindow(parentPosition);
        float left = unit(x), top = unit(y);
        int pixelLeft = Math.round(left * surface.getWidth());
        int pixelTop = Math.round(top * surface.getHeight());
        int pixelWidth = Math.max(0, Math.round(unit(left + unit(width)) * surface.getWidth()) - pixelLeft);
        int pixelHeight = Math.max(0, Math.round(unit(top + unit(height)) * surface.getHeight()) - pixelTop);
        FrameLayout.LayoutParams params = (FrameLayout.LayoutParams) scroller.getLayoutParams();
        int marginLeft = surfacePosition[0] - parentPosition[0] + pixelLeft;
        int marginTop = surfacePosition[1] - parentPosition[1] + pixelTop;
        if (params.width != pixelWidth || params.height != pixelHeight ||
                params.leftMargin != marginLeft || params.topMargin != marginTop) {
            params.width = pixelWidth; params.height = pixelHeight;
            params.leftMargin = marginLeft; params.topMargin = marginTop;
            scroller.setLayoutParams(params);
        }
        focusFirstRange();
    }

    private static int argb(int rgba) { return (rgba >>> 8) | (rgba << 24); }

    public void setAppearance(int foreground, int background, int highlight, float fontSize) {
        post(() -> appearance(foreground, background, highlight, fontSize));
    }

    private void appearance(int foreground, int background, int highlight, float fontSize) {
        text.setTextColor(argb(foreground));
        scroller.setBackgroundColor(argb(background));
        text.setTextSize(TypedValue.COMPLEX_UNIT_SP,
                Float.isFinite(fontSize) && fontSize > 0 ? fontSize : 16);
        highlightColor = argb(highlight);
        paintRanges();
    }

    public void highlight(int[] endpoints, boolean focus) {
        post(() -> {
            ranges = endpoints;
            paintRanges();
            focusOffset = focus && ranges.length >= 2 ? ranges[0] : -1;
            focusFirstRange();
        });
    }

    private void paintRanges() {
        Spannable content = (Spannable) text.getText();
        for (BackgroundColorSpan span : content.getSpans(0, content.length(), BackgroundColorSpan.class))
            content.removeSpan(span);
        for (int i = 0; i + 1 < ranges.length; i += 2) {
            int start = ranges[i], end = ranges[i + 1];
            if (start >= 0 && start < end && end <= content.length())
                content.setSpan(new BackgroundColorSpan(highlightColor), start, end,
                        Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
    }

    private void focusFirstRange() {
        if (focusOffset < 0 || text == null || text.isLayoutRequested() || scroller.isLayoutRequested()) return;
        Layout layout = text.getLayout();
        if (layout == null || scroller.getHeight() == 0) return;
        if (focusOffset <= text.length()) {
            int line = layout.getLineForOffset(focusOffset);
            scroller.scrollTo(0, layout.getLineTop(line) + text.getTotalPaddingTop());
        }
        focusOffset = -1;
    }

    public void scroll(int action) {
        post(() -> {
            focusOffset = -1;
            int line = text.getLineHeight();
            int page = Math.max(line, scroller.getHeight() - line);
            switch (action) {
                case 0: scroller.scrollBy(0, -line); break;
                case 1: scroller.scrollBy(0, line); break;
                case 2: scroller.scrollBy(0, -page); break;
                case 3: scroller.scrollBy(0, page); break;
                case 4: scroller.scrollTo(0, 0); break;
                case 5: scroller.scrollTo(0, text.getHeight()); break;
                default: break;
            }
        });
    }

    // Called only on the UI thread, before SDL sees Back/Escape. Dismissing a
    // native selection menu must not also close the source (or leave the chart).
    boolean handleBack() {
        if (closed.get()) return true;
        if (selectionMode != null) {
            selectionMode.finish();
            Selection.removeSelection((Spannable) text.getText());
        } else if (text != null && text.hasSelection()) {
            Selection.removeSelection((Spannable) text.getText());
        } else requestDismiss(id);
        return true;
    }

    public void close() {
        if (closed.getAndSet(true)) return;
        handler.removeCallbacksAndMessages(this);
        activity.runOnUiThread(this::remove);
    }

    void activityDestroyed() {
        requestDismiss(id);
        close();
    }

    // Idempotent: a close racing open may schedule removal as well as execute it
    // at the end of open. A retiring document must not steal its successor's focus.
    private void remove() {
        if (selectionMode != null) selectionMode.finish();
        selectionMode = null;
        if (Build.VERSION.SDK_INT >= 33 && backCallback != null) {
            BackApi33.unregister(activity, backCallback);
            backCallback = null;
        }
        if (parent != null && parent.getViewTreeObserver().isAlive())
            parent.getViewTreeObserver().removeOnGlobalLayoutListener(layoutListener);
        if (text != null) {
            text.cancelLongPress();
            text.clearFocus();
        }
        if (scroller != null && scroller.getParent() instanceof ViewGroup)
            ((ViewGroup) scroller.getParent()).removeView(scroller);
        if (activity.nativeDocumentView == this) {
            activity.nativeDocumentView = null;
            if (surface != null && !activity.nativeDocumentDestroyed) surface.requestFocus();
        }
        text = null; scroller = null; parent = null; surface = null;
        ranges = new int[0];
    }

    private static final class BackApi33 {
        static Object register(AsoBMaShowActivity activity, Runnable onBack) {
            android.window.OnBackInvokedCallback callback = onBack::run;
            activity.getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    android.window.OnBackInvokedDispatcher.PRIORITY_OVERLAY, callback);
            return callback;
        }
        static void unregister(AsoBMaShowActivity activity, Object callback) {
            activity.getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(
                    (android.window.OnBackInvokedCallback) callback);
        }
    }
}
