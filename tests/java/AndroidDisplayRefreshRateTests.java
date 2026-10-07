package com.snurhythm.asobmashow;

import android.view.Display;
import android.view.Surface;
import android.view.Window;
import android.os.Build;

public final class AndroidDisplayRefreshRateTests {
    private static Display.Mode mode(int id, int width, int height, float rate) {
        return new Display.Mode(id, width, height, rate);
    }

    private static Window window(Display.Mode current, Display.Mode... supported) {
        Window window = new Window();
        window.manager.display = new Display();
        window.manager.display.current = current;
        window.manager.display.supported = supported;
        return window;
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) {
        Display.Mode sixty = mode(1, 1080, 2400, 60.0f);
        Display.Mode high = mode(2, 1080, 2400, 120.0f);
        Window window;
        Surface surface;
        switch (args[0]) {
        case "same-resolution":
            window = window(sixty, mode(3, 1440, 3200, 144.0f), high,
                    mode(4, 1080, 2400, 90.0f), sixty);
            check(AndroidDisplayRefreshRate.requestHighestRefreshRate(window),
                    "Initial high-refresh preference must be requested");
            check(window.attributes.preferredDisplayModeId == 2,
                    "Must prefer 120 Hz at 1080x2400, not 144 Hz at 1440x3200");
            check(window.attributes.preferredRefreshRate == 120.0f,
                    "Requested refresh rate must match selected mode");
            break;
        case "invalid-rates":
            window = window(sixty, mode(2, 1080, 2400, Float.NaN),
                    mode(3, 1080, 2400, Float.POSITIVE_INFINITY),
                    mode(4, 1080, 2400, -120.0f), mode(5, 1080, 2400, 0.0f));
            AndroidDisplayRefreshRate.requestHighestRefreshRate(window);
            check(window.attributes.preferredDisplayModeId == 1,
                    "Invalid candidate rates must preserve valid current mode");
            window = window(mode(1, 1080, 2400, Float.NaN));
            check(!AndroidDisplayRefreshRate.requestHighestRefreshRate(window)
                            && window.writes == 0,
                    "No request is valid without any finite positive rate");
            break;
        case "repeat":
            window = window(sixty, sixty, high);
            AndroidDisplayRefreshRate.requestHighestRefreshRate(window);
            check(!AndroidDisplayRefreshRate.requestHighestRefreshRate(window),
                    "Repeated focus must not repeat the same preference");
            check(window.writes == 1, "Preference must be written exactly once");
            check(window.manager.display.current == sixty,
                    "Preference must allow the system to retain a lower active rate");
            break;
        case "display-change":
            window = window(sixty, sixty, high);
            AndroidDisplayRefreshRate.requestHighestRefreshRate(window);
            window.manager.display.current = mode(3, 1440, 3200, 60.0f);
            window.manager.display.supported = new Display.Mode[] {
                    high, mode(4, 1440, 3200, 90.0f)};
            check(AndroidDisplayRefreshRate.requestHighestRefreshRate(window),
                    "A changed display resolution needs a fresh preference");
            check(window.attributes.preferredDisplayModeId == 4
                            && window.attributes.preferredRefreshRate == 90.0f,
                    "Must reselect the highest rate at the new resolution");
            break;
        case "unavailable":
            check(!AndroidDisplayRefreshRate.requestHighestRefreshRate(null),
                    "Unavailable window must be ignored");
            window = new Window();
            check(!AndroidDisplayRefreshRate.requestHighestRefreshRate(window),
                    "Unavailable display must be ignored");
            window = window(sixty, high);
            window.manager.display.valid = false;
            check(!AndroidDisplayRefreshRate.requestHighestRefreshRate(window)
                            && window.writes == 0,
                    "Disconnected display must not receive a request");
            break;
        case "equal-rate":
            window = window(high, mode(3, 1080, 2400, 120.0f), high);
            AndroidDisplayRefreshRate.requestHighestRefreshRate(window);
            check(window.attributes.preferredDisplayModeId == 2,
                    "Equal-rate alternatives must not replace current mode");
            break;
        case "surface-modern":
            for (int sdk : new int[]{30, 35, 36}) {
                Build.VERSION.SDK_INT = sdk;
                window = window(sixty, high, mode(3, 1440, 3200, 144.0f));
                surface = new Surface();
                check(AndroidDisplayRefreshRate.requestHighestRefreshRate(window, surface),
                        "Modern Android must receive a frame-rate request");
                check(surface.requests == 1 && surface.requestedRate == 120.0f
                                && surface.requestedCompatibility ==
                                    Surface.FRAME_RATE_COMPATIBILITY_DEFAULT,
                        "Surface must request the highest matching-resolution game rate");
            }
            break;
        case "surface-recreated":
            Build.VERSION.SDK_INT = 35;
            window = window(sixty, high);
            AndroidDisplayRefreshRate.requestHighestRefreshRate(window, new Surface());
            surface = new Surface();
            check(AndroidDisplayRefreshRate.requestHighestRefreshRate(window, surface),
                    "A new surface still needs a vote when window preference matches");
            check(surface.requests == 1 && surface.requestedRate == 120.0f
                            && window.writes == 1,
                    "Surface recreation must not require a window-attribute rewrite");
            break;
        case "surface-legacy":
            for (int sdk : new int[]{28, 29}) {
                Build.VERSION.SDK_INT = sdk;
                window = window(sixty, high);
                surface = new Surface();
                AndroidDisplayRefreshRate.requestHighestRefreshRate(window, surface);
                check(window.attributes.preferredDisplayModeId == 2
                                && surface.requests == 0,
                        "Android 9/10 must retain the supported window-preference path");
            }
            break;
        case "surface-unavailable":
            Build.VERSION.SDK_INT = 35;
            window = window(sixty, high);
            AndroidDisplayRefreshRate.requestHighestRefreshRate(window, null);
            surface = new Surface();
            surface.valid = false;
            check(!AndroidDisplayRefreshRate.requestHighestRefreshRate(window, surface)
                            && surface.requests == 0,
                    "Invalid surface cannot receive a request");
            surface.valid = true;
            surface.releaseOnRequest = true;
            check(!AndroidDisplayRefreshRate.requestHighestRefreshRate(window, surface),
                    "Surface release racing a request must not crash the activity");
            window = window(mode(1, 1080, 2400, Float.NaN));
            surface = new Surface();
            check(!AndroidDisplayRefreshRate.requestHighestRefreshRate(window, surface)
                            && surface.requests == 0,
                    "No invalid refresh rate may reach the Surface API");
            break;
        default:
            throw new AssertionError("Unknown scenario: " + args[0]);
        }
    }
}
