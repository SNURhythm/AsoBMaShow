"""Compile complete Settings archive launch/completion/shutdown methods."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/scene/SettingsSceneProfiles.cpp").read_text()
    signatures = (
        "bool cleanupProfileImportTemporaryDocument(",
        "bool SettingsScene::startProfileArchiveTask(",
        "void SettingsScene::applyPendingProfileArchiveCompletion()",
        "void SettingsScene::applyPendingProfileDocumentHandoff()",
        "void SettingsScene::stopProfileArchiveWork()",
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n\n".join(extract(source, value) for value in signatures) + "\n")


if __name__ == "__main__":
    main()
