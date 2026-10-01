import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract

parser = argparse.ArgumentParser()
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
source = (args.root / "src/scene/play/GamePlayScene.cpp").read_text()
method = extract(source, "void GamePlayScene::showGuidedAccessReminder()")
method = method.replace("GamePlayScene::", "ReminderUIFixture::", 1)
# Supply native window geometry on the host while keeping real View/TextView/Yoga.
method = method.replace("#if TARGET_OS_IOS || TARGET_OS_SIMULATOR", "#if 1")
args.output.write_text(method + "\n")
