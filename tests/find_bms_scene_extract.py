"""Compile complete Find BMS scene launch/delivery methods and progress policy."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/scene/MainMenuScene.cpp").read_text()
    signatures = [
        "std::string findBmsTitleSearchQuery(",
        "bool messageStartsWith(",
        "double progressRatio(",
        "std::string\nfindBmsProgressEventDisplayText(",
        "bool shouldReplaceFindBmsLogLine(",
        "double findBmsProgressFractionFor(",
        "void MainMenuScene::showFindBmsModal(",
        "void MainMenuScene::startFindBmsCandidateDownload(",
        "void MainMenuScene::startFindBmsPendingArtifactResolution(",
        "void MainMenuScene::hideFindBmsModal()",
        "void MainMenuScene::applyFindBmsUpdates()",
    ]
    methods = "\n\n".join(extract(source, item) for item in signatures)
    cancel = extract(source, "findBmsCloseButton->setOnClickListener([this]()")
    methods += "\nvoid MainMenuScene::cancelFindBms() " + cancel[cancel.index("{"):]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(methods + "\n")


if __name__ == "__main__":
    main()
