#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import sys


root = (
    pathlib.Path(sys.argv[1])
    if len(sys.argv) > 1
    else pathlib.Path(__file__).parent.parent
).resolve()
main_menu_path = root / "src/scene/MainMenuScene.cpp"
main_menu_header_path = root / "src/scene/MainMenuScene.h"
uploads_scene_path = root / "src/scene/IrUploadsScene.cpp"
preparation_task_path = root / "src/scene/IrUploadPreparationTask.cpp"
music_select_path = root / "src/scene/MusicSelectScene.cpp"

main_menu = (
    main_menu_path.read_text(encoding="utf-8") if main_menu_path.is_file() else ""
)
main_menu_header = (
    main_menu_header_path.read_text(encoding="utf-8")
    if main_menu_header_path.is_file()
    else ""
)
uploads_scene = (
    uploads_scene_path.read_text(encoding="utf-8")
    if uploads_scene_path.is_file()
    else ""
)
preparation_task = (
    preparation_task_path.read_text(encoding="utf-8")
    if preparation_task_path.is_file()
    else ""
)
music_select = (
    music_select_path.read_text(encoding="utf-8")
    if music_select_path.is_file()
    else ""
)
failures: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def reject(condition: bool, message: str) -> None:
    if condition:
        failures.append(message)


def function_body(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        return ""
    opening = source.find("{", start)
    if opening < 0:
        return ""
    depth = 1
    for position in range(opening + 1, len(source)):
        if source[position] == "{":
            depth += 1
        elif source[position] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:position]
    return ""


main_menu_tabs = (
    function_body(main_menu, "void MainMenuScene::selectTab(Tab tab)")
    + function_body(main_menu, "void MainMenuScene::completeTabSwitch(Tab tab)")
)
require(
    "std::unique_ptr<IrUploadsScene> irTab_" in main_menu_header
    and "tab == Tab::Ir && !irTab_" in main_menu_tabs
    and "irTab_ = std::make_unique<IrUploadsScene>(context)" in main_menu_tabs,
    "the built-in menu owns one lazy IR tab instance",
)
configure_position = main_menu_tabs.find("irTab_->configureTab(inset, returnToPlay, this)")
init_position = main_menu_tabs.find("irTab_->init()")
require(
    0 <= configure_position < init_position
    and "selectTab(Tab::Play)" in main_menu_tabs,
    "the IR tab reserves the shell header and closes to PLAY before initialization",
)
reject(
    "changeScene(" in main_menu_tabs,
    "selecting a built-in tab must not transition SceneManager scenes",
)
main_menu_update = function_body(main_menu, "void MainMenuScene::update(float dt)")
require(
    re.search(r"if\s*\(irTab_\)\s*\{[^}]*irTab_->update\(dt\);"
              r"[^}]*irTab_->handleDeferred\(\);", main_menu_update) is not None,
    "the shell keeps IR preparation and deferred callbacks progressing while hidden",
)
main_menu_pause = function_body(main_menu, "void MainMenuScene::onPause()")
reject(
    "irTab_->onProfileWillChange()" in main_menu_pause,
    "ordinary scene pauses must not cancel retained IR preparation",
)
profile_blocker = function_body(main_menu, "auto profileOperationBlocker =")
require(
    "irTab_->profileSwitchBlocker()" in profile_blocker,
    "profile replacement must consult retained IR preparation",
)
music_select_handler_start = music_select.find(
    "void MusicSelectScene::openIrUploads()"
)
music_select_handler_end = music_select.find(
    "void MusicSelectScene::openSettings()", music_select_handler_start
)
music_select_handler = music_select[
    music_select_handler_start:music_select_handler_end
]
require(
    "std::make_unique<IrUploadsScene>" in music_select_handler
    and "SceneReturnTarget::Retained(" in music_select_handler
    and "reactivateSkinOnResume_ = true" in music_select_handler,
    "skinned Song Select toolbar opens the same retained IR Uploads scene",
)
require(
    "enqueueManualBatch" in uploads_scene,
    "the page uses one service batch boundary",
)
require(
    "SettingsDestination::Ir" in uploads_scene,
    "configuration targets the IR tab",
)
require(
    "returnTarget_.notifySettingsWillOpen()" in uploads_scene,
    "indirect IR Settings visits notify the retained Song Select scene",
)
require(
    "ListIrUploadCandidates" in uploads_scene
    and "nextBeforeModernChartResultId" in uploads_scene,
    "the page reads snapshot-backed modern candidates through the paged "
    "repository boundary",
)
reject(
    "ListIrUploadCandidateReplays" in uploads_scene
    or "SelectChartMetaByPaths" in uploads_scene,
    "the page must not hydrate replay rows or chart metadata",
)
reject(
    "enqueueManual(" in uploads_scene,
    "the page must not queue selections one by one",
)
require(
    "loadDiagnostic = page.diagnostic" in uploads_scene,
    "IR Uploads forwards durable result/snapshot agreement diagnostics",
)
reject(
    "This saved result has no verifiable IR proof." in uploads_scene,
    "IR Uploads must not replace proof analysis with a generic rejection",
)

verify_position = uploads_scene.find("submissionForIrUploadCandidate")
batch_position = uploads_scene.find("executeIrSavedResultBatchUpload", verify_position)
enqueue_position = uploads_scene.find("enqueueManualBatch", batch_position)
require(
    verify_position >= 0
    and batch_position > verify_position
    and enqueue_position > batch_position
    and uploads_scene.count("enqueueManualBatch") == 1,
    "stored result agreement must precede exactly one batch enqueue",
)
reject(
    "LoadReplayResult" in uploads_scene
    or "result_recall::BuildChartResult" in uploads_scene,
    "manual IR upload must not reconstruct a replay",
)
settings_action = function_body(uploads_scene, "void IrUploadsScene::openIrSettings()")
require(
    "openIrSettingsButton->setOnClickListener" in uploads_scene
    and re.search(r"std::make_unique<SettingsScene>\(\s*context,\s*"
                  r"SettingsDestination::Ir,", settings_action) is not None,
    "Open IR Settings must be a clickable direct navigation action",
)
require(
    re.search(r"embeddedTab\s*\?\s*SceneReturnTarget::Retained\(tabSettingsReturnOwner\)"
              r"\s*:\s*returnTarget_\),\s*embeddedTab\);", settings_action) is not None,
    "embedded IR Settings must retain the shell and return to it, while standalone uses its return target",
)
back_action = function_body(uploads_scene, "void IrUploadsScene::goBack()")
require(
    re.search(r"if\s*\(embeddedTab\)\s*\{\s*if\s*\(tabClose\)\s*tabClose\(\);"
              r"\s*return;\s*\}\s*stopPreparation\(\);", back_action) is not None
    and "returnToScene(*context.sceneManager, returnTarget_)" in back_action,
    "embedded Back closes the tab without cancelling preparation; standalone Back stops work and returns",
)
tab_pause = function_body(uploads_scene, "void IrUploadsScene::onPause()")
require(
    "onInputQueueOverflow()" in tab_pause
    and "stopPreparation()" not in tab_pause,
    "normal tab pauses retire pointer input while preserving preparation",
)
require(
    "preparationTask.stopAndWait()" in uploads_scene
    and "stopPreparation()" in function_body(uploads_scene, "void IrUploadsScene::cleanupScene()")
    and "stopPreparation()" in function_body(uploads_scene, "void IrUploadsScene::onProfileWillChange()"),
    "cleanup and profile changes must stop and join local preparation",
)
shutdown = preparation_task[preparation_task.find("void PreparationTask::stopAndWait()") :]
gate_cancel = shutdown.find("enqueueGate_->requestCancellation()")
thread_stop = shutdown.find("worker_.request_stop()")
thread_join = shutdown.find("worker_.join()")
require(
    0 <= gate_cancel < thread_stop < thread_join,
    "preparation shutdown must cancel the durable gate before stopping and joining the worker",
)

if failures:
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("IR uploads flow audit passed")
