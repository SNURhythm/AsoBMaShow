"""Extract native ingress methods unchanged for SDL-to-worker host coverage."""
import argparse
from pathlib import Path
from gameplay_terminal_scene_extract import extract

parser = argparse.ArgumentParser()
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
source = (args.root / "src/scene/play/GamePlayScene.cpp").read_text()
methods = ("  void populateImmutableHit(", "  static bool emitTouchInput(",
           "  static void consumeTouchSampleLocked(", "  static void sdlTouchSink(")
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text("\n\n".join(extract(source, signature) for signature in methods))
