"""Compile complete production best-replay scene methods in a small fixture."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/scene/play/GamePlayScene.cpp").read_text()
    signatures = [
        "void GamePlayScene::startBestReplayLoad(",
        "void GamePlayScene::applyPendingBestReplay()",
        "void GamePlayScene::applyLoadedBestReplay(",
        "void GamePlayScene::stopBestReplayLoad()",
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n\n".join(extract(source, item) for item in signatures) + "\n")


if __name__ == "__main__":
    main()
