import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def generate_clip(workspace):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("skin movie clock fixture requires the ffmpeg CLI")
    workspace.mkdir(parents=True, exist_ok=True)
    raw = workspace / "timestamp-coded.yuv"
    clip = workspace / "timestamp-coded.mkv"
    frames = b"".join(
        bytes([16 + 8 * frame]) * 256 + bytes([128]) * 128
        for frame in range(20)
    )
    raw.write_bytes(frames)
    subprocess.run(
        [ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-y",
         "-f", "rawvideo", "-pixel_format", "yuv420p", "-video_size", "16x16",
         "-framerate", "20", "-i", str(raw), "-frames:v", "20", "-an",
         "-c:v", "ffv1", "-level", "3", "-g", "1", "-threads", "1", str(clip)],
        check=True, timeout=10,
    )
    decoded = subprocess.run(
        [ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin",
         "-i", str(clip), "-frames:v", "20", "-f", "rawvideo",
         "-pix_fmt", "yuv420p", "-threads", "1", "pipe:1"],
        check=True, timeout=10, capture_output=True,
    ).stdout
    if decoded != frames:
        raise RuntimeError("fixture failed exact decoded timestamp-marker check")
    print(f"verified {clip}: 20 frames, 16x16, 1000 ms, {clip.stat().st_size} bytes",
          flush=True)
    return clip


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--prepare-only", type=Path)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--scenario", choices=["immediate", "delayed", "seek", "cleanup"])
    parser.add_argument("--workspace", type=Path)
    args = parser.parse_args()
    if args.prepare_only:
        generate_clip(args.prepare_only)
        return
    if not args.binary or not args.scenario or not args.workspace:
        parser.error("execution requires --binary, --scenario and --workspace")
    args.workspace.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="movie-clock-", dir=args.workspace) as root:
        clip = generate_clip(Path(root))
        subprocess.run([str(args.binary.resolve()), str(clip.resolve()), args.scenario],
                       check=True, timeout=30)


if __name__ == "__main__":
    main()
