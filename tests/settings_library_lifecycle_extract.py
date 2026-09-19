"""Compile the complete production Settings destructor in a lifecycle fixture."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/scene/SettingsScenePreview.cpp").read_text()
    methods = extract(source, "SettingsScene::~SettingsScene()")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(methods + "\n")


if __name__ == "__main__":
    main()
