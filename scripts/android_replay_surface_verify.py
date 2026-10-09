#!/usr/bin/env python3
"""Build and run the real MediaCodec Surface integration test on an arm64 device.

Requires an Android NDK, this project's Android FFmpeg installation, adb, and
host ffmpeg/ffprobe. Does not install or change the app or its settings.
"""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def verify_video(path, width, height, fps):
    metadata = json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-select_streams", "v:0", "-show_streams",
        "-of", "json", str(path)]))["streams"][0]
    assert (metadata["width"], metadata["height"]) == (width, height), metadata
    assert metadata["r_frame_rate"] == f"{fps}/1", metadata
    assert int(metadata["nb_frames"]) == fps * 2, metadata
    assert abs(float(metadata["duration"]) - 2) < 0.0001, metadata
    process = subprocess.Popen([
        "ffmpeg", "-v", "error", "-i", str(path), "-an", "-f", "rawvideo",
        "-pix_fmt", "rgb24", "-"], stdout=subprocess.PIPE)
    try:
        for index in range(fps * 2):
            frame = process.stdout.read(width * height * 3)
            assert len(frame) == width * height * 3, ("incomplete frame", index)
            samples = [
                (width // 4, height // 4, (255, 0, 0)),
                (3 * width // 4, height // 4, (0, 255, 0)),
                (width // 4, 3 * height // 4, (0, 0, 255)),
                (3 * width // 4, 3 * height // 4, (255, 255, 255)),
                (8, 8, (255, 255, 255) if index % 2 else (0, 0, 0)),
            ]
            for x, y, expected in samples:
                offset = (y * width + x) * 3
                actual = tuple(frame[offset:offset + 3])
                assert max(abs(a - b) for a, b in zip(actual, expected)) <= 16, (
                    "color/orientation/order", index, x, y, actual, expected)
            for x in range(16, width - 16):
                offset = ((height // 2) * width + x) * 3
                actual = frame[offset]
                assert (actual > 128) == (x % 2 == 1), (
                    "one-pixel stripe", index, x, actual)
        assert not process.stdout.read(1), "extra frames"
        assert process.wait(timeout=30) == 0, "video decode failed"
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        process.stdout.close()
    print(f"PASS: {width}x{height}/{fps}fps; colors, orientation, every frame, thin lines, duration")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--ffmpeg-prefix", type=Path, required=True)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--codec", help="Optional device codec name; otherwise use platform default")
    args = parser.parse_args()
    compiler = next((args.ndk / "toolchains/llvm/prebuilt").glob(
        "*/bin/aarch64-linux-android28-clang++"))
    adb = [args.adb, "-s", args.serial]
    with tempfile.TemporaryDirectory(prefix="asobmashow-replay-surface-") as directory:
        temporary = Path(directory)
        executable = temporary / "surface-test"
        subprocess.run([
            str(compiler), "-O2", "-std=c++23", "-static-libstdc++",
            "-I" + str(ROOT / "src"), "-I" + str(args.ffmpeg_prefix / "include"),
            str(ROOT / "tests/android_replay_surface_tests.cpp"),
            "-L" + str(args.ffmpeg_prefix / "lib"), "-lavformat", "-lavcodec",
            "-lswresample", "-lavutil", "-lx264", "-lm", "-ldl", "-landroid",
            "-lmediandk", "-lEGL", "-lGLESv2", "-llog", "-o", str(executable)], check=True)
        remote = "/data/local/tmp/" + temporary.name
        try:
            subprocess.run([*adb, "push", str(executable), remote], check=True)
            for width, height, fps in [(320, 180, 60), (2400, 1080, 60), (2400, 1080, 120)]:
                command = [remote, remote + ".mp4", str(width), str(height), str(fps)]
                if args.codec:
                    command.append(args.codec)
                subprocess.run([*adb, "shell", *command], check=True, timeout=60)
                video = temporary / "output.mp4"
                subprocess.run([*adb, "pull", remote + ".mp4", str(video)], check=True)
                verify_video(video, width, height, fps)
        finally:
            subprocess.run([*adb, "shell", "rm", "-f", remote, remote + ".mp4"], check=False)


if __name__ == "__main__":
    main()
