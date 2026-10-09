package com.snurhythm.asobmashow;

import android.media.MediaCodecInfo;
import android.media.MediaCodecList;
import android.os.Build;

import java.util.Locale;

final class AndroidReplayCodec {
    private AndroidReplayCodec() {}

    static String findEncoder(int width, int height, int fps, int bitrate) {
        if (width <= 0 || height <= 0 || fps <= 0 || bitrate <= 0) return "";
        try {
            for (MediaCodecInfo codec :
                    new MediaCodecList(MediaCodecList.REGULAR_CODECS).getCodecInfos()) {
                try {
                    if (!codec.isEncoder() || !isHardware(codec)) continue;
                    for (String type : codec.getSupportedTypes()) {
                        if (!"video/avc".equalsIgnoreCase(type)) continue;
                        MediaCodecInfo.CodecCapabilities caps = codec.getCapabilitiesForType(type);
                        boolean surface = false;
                        for (int format : caps.colorFormats) {
                            if (format == MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface) {
                                surface = true;
                                break;
                            }
                        }
                        MediaCodecInfo.VideoCapabilities video = caps.getVideoCapabilities();
                        if (surface && video != null &&
                                video.areSizeAndRateSupported(width, height, fps) &&
                                video.getBitrateRange().contains(bitrate)) return codec.getName();
                    }
                } catch (RuntimeException ignored) {
                    // Some vendor entries advertise capabilities they cannot query.
                }
            }
        } catch (RuntimeException ignored) {
            // Unavailable media services leave the software export path usable.
        }
        return "";
    }

    private static boolean isHardware(MediaCodecInfo codec) {
        if (Build.VERSION.SDK_INT >= 29) return codec.isHardwareAccelerated();
        // Android 9 has no hardware flag; exclude its platform software codecs.
        String name = codec.getName().toLowerCase(Locale.ROOT);
        return (name.startsWith("omx.") || name.startsWith("c2.")) &&
                !name.startsWith("omx.google.") && !name.startsWith("c2.android.") &&
                !name.startsWith("c2.google.") && !name.contains(".sw.");
    }
}
