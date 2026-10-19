package com.snurhythm.asobmashow;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.os.Build;

import java.util.Locale;

final class AndroidStorageCodec {
    private AndroidStorageCodec() {}

    static String findEncoder(String mime, int width, int height, int fps, int bitrate) {
        if (mime == null || mime.isEmpty() || width <= 0 || height <= 0 ||
                fps <= 0 || bitrate <= 0) return "";
        try {
            for (MediaCodecInfo codec :
                    new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
                try {
                    MediaCodecInfo.CodecCapabilities caps = capabilities(codec, mime);
                    if (caps == null || pixelFormat(caps).isEmpty()) continue;
                    MediaCodecInfo.VideoCapabilities video = caps.getVideoCapabilities();
                    if (video != null && video.areSizeAndRateSupported(width, height, fps) &&
                            video.getBitrateRange().contains(bitrate)) return codec.getName();
                } catch (RuntimeException ignored) {
                    // Continue past broken vendor capability entries.
                }
            }
        } catch (RuntimeException ignored) {
            // Native code can fall back to software when media services are unavailable.
        }
        return "";
    }

    static String getPixelFormat(String codecName, String mime) {
        if (codecName == null || codecName.isEmpty() || mime == null || mime.isEmpty()) return "";
        try {
            for (MediaCodecInfo codec :
                    new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
                try {
                    if (!codecName.equals(codec.getName())) continue;
                    MediaCodecInfo.CodecCapabilities caps = capabilities(codec, mime);
                    if (caps != null) return pixelFormat(caps);
                } catch (RuntimeException ignored) {
                    // A failed re-query must not guess the vendor's buffer layout.
                }
            }
        } catch (RuntimeException ignored) {
            // Match findEncoder's unavailable-media-service fallback.
        }
        return "";
    }

    private static MediaCodecInfo.CodecCapabilities capabilities(MediaCodecInfo codec, String mime) {
        if (!codec.isEncoder() || !isHardware(codec)) return null;
        for (String type : codec.getSupportedTypes()) {
            if (mime.equalsIgnoreCase(type)) return codec.getCapabilitiesForType(type);
        }
        return null;
    }

    private static String pixelFormat(MediaCodecInfo.CodecCapabilities caps) {
        boolean planar = false;
        // FFmpeg's MediaCodec buffer encoder requests explicit 19/21 layouts.
        // Surface and YUV420Flexible alone do not promise either buffer layout.
        for (int format : caps.colorFormats) {
            if (format == MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420SemiPlanar)
                return "nv12";
            if (format == MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar)
                planar = true;
        }
        return planar ? "yuv420p" : "";
    }

    private static boolean isHardware(MediaCodecInfo codec) {
        if (Build.VERSION.SDK_INT >= 29) return codec.isHardwareAccelerated();
        String name = codec.getName().toLowerCase(Locale.ROOT);
        return (name.startsWith("omx.") || name.startsWith("c2.")) &&
                !name.startsWith("omx.google.") && !name.startsWith("c2.android.") &&
                !name.startsWith("c2.google.") && !name.contains(".sw.");
    }
}
