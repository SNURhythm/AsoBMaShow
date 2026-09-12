import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from skin_movie_clock_tests import generate_clip


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    args = parser.parse_args()
    args.workspace.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="visual-catch-up-", dir=args.workspace) as root:
        clip = generate_clip(Path(root))
        playable = clip.with_suffix(".mp4")
        shutil.copyfile(clip, playable)
        (Path(root) / "image.bmp").write_bytes(b"P6\n1 1\n255\n\x22\x44\x66")
        subprocess.run([str(args.binary.resolve()), str(playable.resolve())],
                       check=True, timeout=40)


if __name__ == "__main__":
    main()
