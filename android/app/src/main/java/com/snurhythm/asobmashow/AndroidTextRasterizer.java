package com.snurhythm.asobmashow;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.icu.lang.UCharacter;
import android.icu.lang.UProperty;

import java.nio.charset.StandardCharsets;

/** Worker-local Canvas rasterization; no Activity or UI-thread dependency. */
public final class AndroidTextRasterizer {
    private AndroidTextRasterizer() {}

    public static native boolean initializeNative();

    // Header: width, height, ascent, advance, offsetX. Pixels are straight-alpha ARGB,
    // as returned by Bitmap.getPixels (not Bitmap's premultiplied storage).
    public static int[] rasterize(byte[] utf8, int size, int color,
                                  long maximumBytes, boolean colorOnly,
                                  boolean metricsOnly) {
        if (utf8 == null || utf8.length == 0 || utf8.length > 65536
                || size <= 0 || size > 8192 || maximumBytes < 4) return null;
        Bitmap bitmap = null;
        try {
            String text = new String(utf8, StandardCharsets.UTF_8);
            Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
            paint.setTypeface(Typeface.DEFAULT);
            paint.setTextSize(size);
            paint.setColor(color);
            if (colorOnly) {
                if (text.codePointCount(0, text.length()) != 1) return null;
                int codepoint = text.codePointAt(0);
                if (!UCharacter.hasBinaryProperty(codepoint, UProperty.EMOJI)) return null;
                if (!UCharacter.hasBinaryProperty(codepoint, UProperty.EMOJI_PRESENTATION)) {
                    text += "\ufe0f";
                }
                if (!paint.hasGlyph(text)) return null;
            }
            float measured = paint.measureText(text);
            Paint.FontMetricsInt metrics = paint.getFontMetricsInt();
            if (!Float.isFinite(measured) || measured < 0 || measured > 8192) return null;
            int advance = (int) Math.ceil(measured);
            Rect ink = new Rect();
            paint.getTextBounds(text, 0, text.length(), ink);
            int left = Math.min(0, ink.left);
            int right = Math.max(advance, ink.right);
            int top = Math.min(metrics.top, ink.top);
            int bottom = Math.max(metrics.bottom, ink.bottom);
            long inkWidth = Math.max(1L, (long) right - left);
            long inkHeight = (long) bottom - top;
            if (inkWidth > 8192 || inkHeight <= 0 || inkHeight > 8192) return null;
            int width = (int) inkWidth;
            int height = (int) inkHeight;
            long pixels = (long) width * height;
            if (height <= 0 || height > 8192 || pixels > Math.min(maximumBytes, 32L * 1024 * 1024) / 4) {
                return null;
            }
            int[] result = new int[5 + (metricsOnly ? 0 : (int) pixels)];
            result[0] = width;
            result[1] = height;
            result[2] = -top;
            result[3] = advance;
            result[4] = left;
            if (!metricsOnly) {
                bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
                new Canvas(bitmap).drawText(text, -left, -top, paint);
                bitmap.getPixels(result, 5, width, 0, 0, width, height);
            }
            return result;
        } catch (RuntimeException | OutOfMemoryError error) {
            return null;
        } finally {
            if (bitmap != null) bitmap.recycle();
        }
    }
}
