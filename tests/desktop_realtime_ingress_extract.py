"""Exercise the production event watch with a mutable presentation fixture."""
import argparse
from pathlib import Path
from gameplay_terminal_scene_extract import extract

parser = argparse.ArgumentParser()
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
source = (args.root / "src/scene/play/GamePlayScene.cpp").read_text()
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text("\n\n".join(extract(source, signature) for signature in
    ("  void publishKeyboardTextFocus(", "  static bool SDLCALL sdlInputWatch(")))
