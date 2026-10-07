package com.snurhythm.asobmashow;

import android.os.Build;
import android.view.Display;
import android.view.Surface;
import android.view.Window;
import android.view.WindowManager;

/** Requests a refresh-rate preference without changing the display resolution. */
final class AndroidDisplayRefreshRate {
    private AndroidDisplayRefreshRate() {}

    static boolean requestHighestRefreshRate(Window window) {
        return requestHighestRefreshRate(window, null);
    }

    @SuppressWarnings("deprecation")
    static boolean requestHighestRefreshRate(Window window, Surface surface) {
        if (window == null || window.getWindowManager() == null) return false;
        // This WindowManager is attached to the Activity's display. The API is
        // also available on Android 9, the app's oldest supported release.
        Display display = window.getWindowManager().getDefaultDisplay();
        if (display == null || !display.isValid()) return false;
        Display.Mode current = display.getMode();
        if (current == null || current.getPhysicalWidth() <= 0
                || current.getPhysicalHeight() <= 0) return false;

        Display.Mode best = validRate(current.getRefreshRate()) ? current : null;
        for (Display.Mode candidate : display.getSupportedModes()) {
            if (candidate.getPhysicalWidth() != current.getPhysicalWidth()
                    || candidate.getPhysicalHeight() != current.getPhysicalHeight()
                    || !validRate(candidate.getRefreshRate())) continue;
            if (best == null || candidate.getRefreshRate() > best.getRefreshRate()) {
                best = candidate;
            }
        }
        if (best == null) return false;

        WindowManager.LayoutParams attributes = window.getAttributes();
        boolean requested = false;
        // A preference, not a forced mode: Android still applies power-saving,
        // thermal and other display-policy limits. Do not retry against the
        // observed active rate when the same preference is already installed.
        if (attributes.preferredDisplayModeId != best.getModeId()
                || attributes.preferredRefreshRate != best.getRefreshRate()) {
            attributes.preferredDisplayModeId = best.getModeId();
            attributes.preferredRefreshRate = best.getRefreshRate();
            window.setAttributes(attributes);
            requested = true;
        }
        // Android 15 games need a Frame Rate API request to opt out of the
        // default 60 Hz policy. Reapply it to newly created SDL surfaces even
        // when the window's legacy preference is already correct.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                && surface != null && surface.isValid()) {
            try {
                surface.setFrameRate(best.getRefreshRate(),
                        Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
                requested = true;
            } catch (IllegalStateException ignored) {
                // A surface can be released between validation and this call.
                // Its replacement receives the preference in surfaceCreated.
            }
        }
        return requested;
    }

    private static boolean validRate(float rate) {
        return Float.isFinite(rate) && rate > 0.0f;
    }
}
