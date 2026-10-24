package com.snurhythm.asobmashow;

/** Runs against an assembled APK through app_process; see android_emoji_verify.sh. */
public final class EmojiDeviceTest {
    private static native int verify(String directory, int unavailableEmoji);

    private static int[] raster(String text, boolean singleGlyph) throws Exception {
        return (int[]) Class.forName("com.snurhythm.asobmashow.AndroidTextRasterizer")
                .getMethod("rasterize", byte[].class, int.class, int.class,
                           long.class, boolean.class, boolean.class)
                .invoke(null, text.getBytes(java.nio.charset.StandardCharsets.UTF_8),
                        32, -1, 1024L * 1024, singleGlyph, false);
    }

    private static void verifyInkBounds() throws Exception {
        android.graphics.Paint paint = new android.graphics.Paint(
                android.graphics.Paint.ANTI_ALIAS_FLAG | android.graphics.Paint.SUBPIXEL_TEXT_FLAG);
        paint.setTypeface(android.graphics.Typeface.DEFAULT);
        paint.setTextSize(32);
        paint.setColor(-1);
        boolean exercisedOverhang = false;
        for (String text : new String[]{"🙂", "A\u030d\u030d\u030d\u030d", "བོད་སྐད་", "အင်္ဂါ", "لأّ", "\u0f73"}) {
            int[] image = raster(text, false);
            if (image == null) throw new AssertionError("Native raster missing: " + text);
            exercisedOverhang |= image[4] < 0 || image[2] > -paint.getFontMetricsInt().top;
            android.graphics.Bitmap reference = android.graphics.Bitmap.createBitmap(
                    512, 256, android.graphics.Bitmap.Config.ARGB_8888);
            try {
                new android.graphics.Canvas(reference).drawText(text, 128, 128, paint);
                int[] pixels = new int[512 * 256];
                reference.getPixels(pixels, 0, 512, 0, 0, 512, 256);
                int referenceInk = 0, rasterInk = 0;
                for (int i = 5; i < image.length; ++i) if ((image[i] >>> 24) != 0) ++rasterInk;
                for (int y = 0; y < 256; ++y) for (int x = 0; x < 512; ++x) {
                    int pixel = pixels[y * 512 + x];
                    if ((pixel >>> 24) == 0) continue;
                    ++referenceInk;
                    int rx = x - 128 - image[4], ry = y - 128 + image[2];
                    if (rx < 0 || rx >= image[0] || ry < 0 || ry >= image[1]
                            || image[5 + ry * image[0] + rx] != pixel) {
                        throw new AssertionError("Clipped or misplaced ink: " + text);
                    }
                }
                if (referenceInk == 0 || referenceInk != rasterInk) {
                    throw new AssertionError("Raster differs from padded reference: " + text);
                }
            } finally {
                reference.recycle();
            }
        }
        if (!exercisedOverhang) throw new AssertionError("No overhanging test glyph on this device");
        System.out.println("PASS: full ink matches padded Canvas references");
    }

    private static int verifyMissingEmoji() throws Exception {
        android.graphics.Paint paint = new android.graphics.Paint();
        paint.setTextSize(32);
        for (int codepoint = 128; codepoint < 0x20000; ++codepoint) {
            if (!android.icu.lang.UCharacter.hasBinaryProperty(codepoint, android.icu.lang.UProperty.EMOJI)) continue;
            String text = new String(Character.toChars(codepoint));
            String presented = text;
            if (!android.icu.lang.UCharacter.hasBinaryProperty(codepoint, android.icu.lang.UProperty.EMOJI_PRESENTATION)) {
                presented += "\ufe0f";
            }
            if (!paint.hasGlyph(presented)) {
                if (raster(text, true) != null) throw new AssertionError("Unavailable emoji accepted");
                System.out.printf("PASS: unavailable recognized emoji U+%X rejected%n", codepoint);
                return codepoint;
            }
        }
        System.out.println("SKIP: device font covers all ICU-recognized emoji");
        return -1;
    }

    // SDL resolves relative bundled-font paths through the Activity files and
    // asset APIs. Supply isolated extracted APK files, never application data.
    public static final class TestActivity extends android.app.Activity {
        private final java.io.File directory;
        private final android.content.res.AssetManager assets;
        TestActivity(String path, android.content.res.AssetManager manager) {
            directory = new java.io.File(path);
            assets = manager;
        }
        @Override public java.io.File getFilesDir() { return directory; }
        @Override public android.content.res.AssetManager getAssets() { return assets; }
    }

    public static void main(String[] arguments) throws Exception {
        // app_process bypasses ActivityThread's application/font initialization.
        // Android 12+ initializes its serialized font map there; older releases
        // initialize Typeface statically. This reflection is test-runner only.
        try {
            android.graphics.Typeface.class.getDeclaredMethod(
                    "setSystemFontMap", android.os.SharedMemory.class).invoke(null, new Object[]{null});
        } catch (NoSuchMethodException oldAndroid) {
            // No serialized font-map initialization on this platform version.
        }
        String directory = arguments[0];
        for (String library : new String[]{"c++_shared", "SDL3", "SDL3_ttf", "main", "emoji_device_tests"}) {
            System.load(directory + "/lib" + library + ".so");
        }
        if (android.os.Looper.myLooper() == null) android.os.Looper.prepareMainLooper();
        Class<?> activityThread = Class.forName("android.app.ActivityThread");
        Object thread = activityThread.getMethod("systemMain").invoke(null);
        android.content.Context context = (android.content.Context)
                activityThread.getMethod("getSystemContext").invoke(thread);
        Class.forName("org.libsdl.app.SDL").getMethod("setContext", android.app.Activity.class)
                .invoke(null, new TestActivity(directory, context.getAssets()));
        Class.forName("org.libsdl.app.SDLActivity").getMethod("nativeSetupJNI").invoke(null);
        boolean initialized = (Boolean) Class.forName("com.snurhythm.asobmashow.AndroidTextRasterizer")
                .getMethod("initializeNative").invoke(null);
        if (!initialized) throw new AssertionError("JNI initialization");
        verifyInkBounds();
        int failures = verify(directory, verifyMissingEmoji());
        if (failures != 0) throw new AssertionError(failures + " emoji checks failed");
        System.out.println("PASS: Android Korean bundled fallback, emoji rendering, atlas baselines and bounds");
    }
}
