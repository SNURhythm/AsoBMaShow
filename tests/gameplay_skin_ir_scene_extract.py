"""Exercise gameplay's production IR polling without audio/rendering services."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/scene/play/GamePlayScene.cpp").read_text()
    fixture = (args.root / "tests/gameplay_skin_ir_scene_fixture.cpp").read_text()
    fixture = fixture.replace("PRODUCTION_GAMEPLAY_IR_UPDATE", extract(
        source, "void GamePlayScene::updateSkinIrTarget()"))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fixture)


if __name__ == "__main__":
    main()
