"""Compile custom touch routes verbatim with controlled UI/media effects."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract

parser = argparse.ArgumentParser()
parser.add_argument("--root", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
fixture = (args.root / "tests/custom_touch_cancel_fixture.cpp").read_text()
methods = []
for path, signatures in (
    ("src/scene/MusicSelectScene.cpp", ("bool MusicSelectScene::queueSkinPointerEvent(",)),
    ("src/scene/MusicPlayerScene.cpp", ("bool MusicPlayerScene::handleProgressSeekEvents(",)),
    ("src/scene/play/GamePlayScene.cpp", (
        "bool GamePlayScene::handleCoursePauseButtonEvent(",
        "void GamePlayScene::beginCoursePauseHold(",
        "void GamePlayScene::cancelCoursePauseHold()",
        "void GamePlayScene::resetCoursePauseHold()",
        "void GamePlayScene::updateCoursePauseHoldProgress(",)),
    ("src/scene/SettingsScenePreview.cpp", ("void SettingsScene::forwardPreviewInputEvent(",)),
    ("src/scene/ResultScene.cpp", (
        "bool ResultScene::queueResultSkinPointerEvent(",
        "EventHandleResult ResultScene::handleEvents(",)),
):
    source = (args.root / path).read_text()
    methods.extend(extract(source, signature) for signature in signatures)
for path, class_name in (
    ("src/scene/ChartViewerScene.cpp", "Viewer"),
    ("src/scene/PracticeAnalyticsView.cpp", "Analytics"),
):
    method = extract((args.root / path).read_text(), "  bool handleEventsImpl(SDL_Event &event) override")
    cancel = extract((args.root / path).read_text(), "  void onPointerInputCancelled() override")
    methods.append(cancel.replace("onPointerInputCancelled(", f"{class_name}::onPointerInputCancelled(", 1)
                   .replace(" override", ""))
    methods.append(method.replace("handleEventsImpl(", f"{class_name}::handleEventsImpl(", 1)
                   .replace(" override", ""))
fixture = fixture.replace("HANDLER_METHODS", "\n\n".join(methods))
ranking = extract((args.root / "src/ir/IrRankingTableViewport.h").read_text(),
                  "class RankingTableViewport")
fixture = fixture.replace("RANKING_CLASS", "namespace ir {\n" + ranking + ";\n}")
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(fixture)
