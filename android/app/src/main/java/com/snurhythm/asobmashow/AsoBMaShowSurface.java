package com.snurhythm.asobmashow;

import android.content.Context;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/** Retains Android sample timing while SDL owns touch and device lifecycles. */
final class AsoBMaShowSurface extends SDLSurface {
    private final AndroidTouchInput.Sink touchSink = new AndroidTouchInput.Sink() {
        @Override public void setTimestamp(long uptimeNanos) {
            nativeSetInputTimestamp(uptimeNanos);
        }
        @Override public void motionTouch(int device, int pointer,
                                               float x, float y, float pressure) {
            SDLActivity.onNativeTouch(device, pointer, MotionEvent.ACTION_MOVE, x, y, pressure);
        }
        @Override public boolean currentTouch(View view, MotionEvent event) {
            return AsoBMaShowSurface.super.onTouch(view, event);
        }
    };

    private final AndroidTouchInput.MotionSink motionSink = new AndroidTouchInput.MotionSink() {
        @Override public void setTimestamp(long uptimeNanos) {
            nativeSetInputTimestamp(uptimeNanos);
        }
        @Override public boolean currentMotion(View view, MotionEvent event) {
            View.OnGenericMotionListener listener = SDLActivity.getMotionListener();
            return listener.onGenericMotion(view, event);
        }
    };

    AsoBMaShowSurface(Context context) {
        super(context);
        setOnGenericMotionListener((view, event) ->
                AndroidTouchInput.dispatchMotion(view, event, motionSink));
    }

    @Override public boolean onCapturedPointerEvent(MotionEvent event) {
        return AndroidTouchInput.dispatchMotion(this, event, motionSink);
    }

    @Override public boolean onTouch(View view, MotionEvent event) {
        return AndroidTouchInput.dispatch(view, event, mWidth, mHeight, touchSink);
    }

    @Override public boolean onKey(View view, int keyCode, KeyEvent event) {
        nativeSetInputTimestamp(event.getEventTime() * 1_000_000L);
        try {
            return super.onKey(view, keyCode, event);
        } finally {
            nativeSetInputTimestamp(0);
        }
    }

    private static native void nativeSetInputTimestamp(long uptimeNanos);
}
