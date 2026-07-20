#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys


root = (
    pathlib.Path(sys.argv[1])
    if len(sys.argv) > 1
    else pathlib.Path(__file__).parent.parent
).resolve()
main_menu_path = root / "src/scene/MainMenuScene.cpp"
uploads_scene_path = root / "src/scene/IrUploadsScene.cpp"

main_menu = (
    main_menu_path.read_text(encoding="utf-8") if main_menu_path.is_file() else ""
)
uploads_scene = (
    uploads_scene_path.read_text(encoding="utf-8")
    if uploads_scene_path.is_file()
    else ""
)
failures: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def reject(condition: bool, message: str) -> None:
    if condition:
        failures.append(message)


require(
    "make_unique<IrUploadsScene>(context)" in main_menu,
    "Song Select opens the dedicated scene",
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
    "ListIrUploadCandidateReplays" in uploads_scene
    and "SelectChartMetaByPaths" in uploads_scene,
    "the page uses explicit batch reads",
)
reject(
    "enqueueManual(" in uploads_scene,
    "the page must not queue selections one by one",
)
require(
    "historicalIrDiagnostic" in uploads_scene,
    "IR Uploads forwards historical proof rejection analysis",
)
reject(
    "This saved result has no verifiable IR proof." in uploads_scene,
    "IR Uploads must not replace proof analysis with a generic rejection",
)

load_position = uploads_scene.find("LoadReplayResult")
rebuild_position = uploads_scene.find("result_recall::BuildChartResult", load_position)
enqueue_position = uploads_scene.find("enqueueManualBatch", rebuild_position)
require(
    load_position >= 0
    and rebuild_position > load_position
    and enqueue_position > rebuild_position
    and uploads_scene.count("enqueueManualBatch") == 1,
    "saved-result reconstruction must precede exactly one batch enqueue",
)
require(
    "openIrSettingsButton->setOnClickListener" in uploads_scene
    and "std::make_unique<SettingsScene>(context," in uploads_scene
    and "SettingsDestination::Ir" in uploads_scene,
    "Open IR Settings must be a clickable direct navigation action",
)
require(
    "request_stop()" in uploads_scene and ".join()" in uploads_scene,
    "Back and cleanup must stop and join local preparation",
)

if failures:
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("IR uploads flow audit passed")
