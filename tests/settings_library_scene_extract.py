"""Compile complete Settings job launch and delivery methods in a small fixture."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/scene/SettingsSceneTables.cpp").read_text()
    signatures = [
        "void SettingsScene::applyPendingDifficultyTableUpdates()",
        "void SettingsScene::addDifficultyTableFromUrl()",
        "void SettingsScene::updateDifficultyTableFromSource(",
        "void SettingsScene::deleteDifficultyTable(",
        "void SettingsScene::hideDifficultyTableImportModal()",
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n\n".join(extract(source, item) for item in signatures) + "\n")


if __name__ == "__main__":
    main()
