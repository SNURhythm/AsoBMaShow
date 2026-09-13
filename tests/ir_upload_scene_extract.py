"""Compile complete IR preparation launch, delivery, and stop scene methods."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/scene/IrUploadsScene.cpp").read_text()
    methods = [extract(source, signature) for signature in [
        "void IrUploadsScene::startUpload()",
        "void IrUploadsScene::applyPreparationUpdates()",
        "void IrUploadsScene::stopPreparation()",
    ]]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n\n".join(methods) + "\n")


if __name__ == "__main__":
    main()
