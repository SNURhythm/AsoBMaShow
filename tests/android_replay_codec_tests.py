"""Exercise hardware encoder selection against controlled Android capabilities."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "android/app/src/main/java/com/snurhythm/asobmashow/AndroidReplayCodec.java"

STUBS = {
    "android/os/Build.java": """
package android.os;
public class Build { public static class VERSION { public static int SDK_INT = 33; } }
""",
    "android/media/MediaCodecList.java": """
package android.media;
public class MediaCodecList {
    public static final int REGULAR_CODECS = 0;
    public static MediaCodecInfo[] codecs;
    public MediaCodecList(int kind) {}
    public MediaCodecInfo[] getCodecInfos() { return codecs; }
}
""",
    "android/media/MediaCodecInfo.java": """
package android.media;
public class MediaCodecInfo {
    public String name;
    public boolean encoder = true, hardware = true, broken;
    public String[] types = {"video/avc"};
    public CodecCapabilities caps = new CodecCapabilities();
    public MediaCodecInfo(String value) { name = value; }
    public boolean isEncoder() { return encoder; }
    public boolean isHardwareAccelerated() {
        if (android.os.Build.VERSION.SDK_INT < 29) throw new AssertionError("API 29 only");
        return hardware;
    }
    public String getName() { return name; }
    public String[] getSupportedTypes() { return types; }
    public CodecCapabilities getCapabilitiesForType(String type) {
        if (broken) throw new IllegalArgumentException("unavailable codec");
        if (!type.equals("video/avc")) throw new AssertionError(type);
        return caps;
    }
    public static class CodecCapabilities {
        public static final int COLOR_FormatSurface = 0x7f000789;
        public int[] colorFormats = {COLOR_FormatSurface};
        public VideoCapabilities video = new VideoCapabilities();
        public VideoCapabilities getVideoCapabilities() { return video; }
    }
    public static class VideoCapabilities {
        public int maxWidth = 4096, maxHeight = 2160, maxRate = 120;
        public BitrateRange bitrate = new BitrateRange();
        public boolean areSizeAndRateSupported(int width, int height, double rate) {
            return width > 0 && height > 0 && rate > 0 && width <= maxWidth &&
                   height <= maxHeight && rate <= maxRate;
        }
        public BitrateRange getBitrateRange() { return bitrate; }
    }
    public static class BitrateRange {
        public int max = 80000000;
        public boolean contains(Integer value) { return value > 0 && value <= max; }
    }
}
""",
}

HARNESS = """
package com.snurhythm.asobmashow;
import android.media.*;
public class ReplayCodecTest {
    static void expect(String expected, MediaCodecInfo... codecs) {
        MediaCodecList.codecs = codecs;
        String actual = AndroidReplayCodec.findEncoder(2400, 1080, 60, 25920000);
        if (!actual.equals(expected)) throw new AssertionError(expected + " != " + actual);
    }
    public static void main(String[] args) {
        MediaCodecInfo software = new MediaCodecInfo("c2.android.avc.encoder");
        software.hardware = false;
        MediaCodecInfo hardware = new MediaCodecInfo("c2.vendor.avc.encoder");
        expect(hardware.name, software, hardware);
        expect("", software);
        MediaCodecInfo decoder = new MediaCodecInfo("decoder");
        decoder.encoder = false;
        MediaCodecInfo hevc = new MediaCodecInfo("hevc");
        hevc.types = new String[]{"video/hevc"};
        MediaCodecInfo small = new MediaCodecInfo("small");
        small.caps.video.maxWidth = 1920;
        MediaCodecInfo slow = new MediaCodecInfo("slow");
        slow.caps.video.maxRate = 30;
        MediaCodecInfo lowBitrate = new MediaCodecInfo("lowBitrate");
        lowBitrate.caps.video.bitrate.max = 8000000;
        MediaCodecInfo noSurface = new MediaCodecInfo("noSurface");
        noSurface.caps.colorFormats = new int[]{19};
        MediaCodecInfo broken = new MediaCodecInfo("broken");
        broken.broken = true;
        expect(hardware.name, decoder, hevc, small, slow, lowBitrate, noSurface, broken, hardware);
        expect("", decoder, hevc, small, slow, lowBitrate, noSurface, broken);
        android.os.Build.VERSION.SDK_INT = 28;
        MediaCodecInfo google = new MediaCodecInfo("OMX.google.h264.encoder");
        expect(hardware.name, software, google, hardware);
        expect("", software, google);
        MediaCodecList.codecs = new MediaCodecInfo[]{hardware};
        if (!AndroidReplayCodec.findEncoder(0, 1080, 60, 25920000).isEmpty())
            throw new AssertionError("invalid dimensions");
    }
}
"""


class AndroidReplayCodecTests(unittest.TestCase):
    def test_hardware_surface_selection_and_software_fallback(self):
        self.assertTrue(JAVA.exists(), "Hardware replay codec selection is missing")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sources = []
            for name, source in STUBS.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source)
                sources.append(str(path))
            harness = root / "ReplayCodecTest.java"
            harness.write_text(HARNESS)
            java_home = os.environ.get("JAVA_HOME")
            javac = str(Path(java_home) / "bin/javac") if java_home else "javac"
            java = str(Path(java_home) / "bin/java") if java_home else "java"
            subprocess.run([javac, "-d", directory, *sources, str(JAVA), str(harness)], check=True)
            subprocess.run([java, "-cp", directory, "com.snurhythm.asobmashow.ReplayCodecTest"], check=True)


if __name__ == "__main__":
    unittest.main()
