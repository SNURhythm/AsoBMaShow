"""Exercise legacy touch routing without starting SDL or a graphics device."""
import argparse
from pathlib import Path
from gameplay_terminal_scene_extract import extract

parser = argparse.ArgumentParser()
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
source = (args.root / "src/input/RhythmInputHandler.cpp").read_text()
signatures = (
    "bool RhythmInputHandler::notifyTouchEvent(",
    "void RhythmInputHandler::discardPendingTouchEvents()",
    "void RhythmInputHandler::setApplicationBackground(bool background)",
    "Vector3 RhythmInputHandler::normalizedTouchToRenderLocation(",
    "bool RhythmInputHandler::isLaneOccupied(",
    "void RhythmInputHandler::beginFingerLane(",
    "void RhythmInputHandler::releaseFingerLane(",
    "void RhythmInputHandler::handleScratchMove(",
    "void RhythmInputHandler::onFingerDown(",
    "void RhythmInputHandler::onFingerUp(",
    "void RhythmInputHandler::onFingerMove(",
    "int RhythmInputHandler::clampLane(",
    "bool RhythmInputHandler::isScratchLane(",
    "int RhythmInputHandler::touchToLaneIndex(",
    "std::optional<int> RhythmInputHandler::touchToLaneIfInside(",
    "int RhythmInputHandler::touchToLane(",
    "void RhythmInputHandler::setTouchLaneLayout(",
    "std::optional<int> RhythmInputHandler::authoredTouchLane(",
)
methods = "\n\n".join(extract(source, signature) for signature in signatures)
scene_source = (args.root / "src/scene/play/GamePlayScene.cpp").read_text()
methods += "\n\n" + "\n\n".join(extract(scene_source, signature) for signature in (
    "bool GamePlayScene::handleTouchInputAtGameplayTime(",
    "void GamePlayScene::cancelLegacyFloatingLaneCoverTouch()",
    "void GamePlayScene::refreshLegacyTouchLayout()",
))
fixture = (args.root / "tests/legacy_skin_touch_fixture.cpp").read_text()
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(fixture.replace("// PRODUCTION_METHODS", methods.replace("TARGET_OS_ANDROID", "fixtureAndroid")))
