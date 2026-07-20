#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys


root = (
    pathlib.Path(sys.argv[1])
    if len(sys.argv) > 1
    else pathlib.Path(__file__).parent.parent
).resolve()
settings_path = root / "src/scene/SettingsSceneLayout.cpp"
main_menu_path = root / "src/scene/MainMenuScene.cpp"

for path in (settings_path, main_menu_path):
    if not path.is_file():
        print(f"FAIL: missing {path.relative_to(root)}", file=sys.stderr)
        raise SystemExit(1)

settings_source = settings_path.read_text(encoding="utf-8")
main_menu_source = main_menu_path.read_text(encoding="utf-8")
failures: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


require(
    "BmsSearchService::kSkipUnarchivingSettingLabel" in settings_source,
    "Settings must use the canonical Find BMS option label",
)
require(
    settings_source.count("findBmsSkipUnarchivingForNonSolidArchives") >= 2,
    "Settings must toggle the persisted Find BMS option",
)
require(
    main_menu_source.count("skipUnarchivingForNonSolidArchives") >= 2,
    "automatic and candidate downloads must capture the option",
)
require(
    "startFindBmsPendingArtifactResolution(" in main_menu_source
    and "BmsSearchPendingArtifactDecision::Keep" in main_menu_source
    and "BmsSearchPendingArtifactDecision::Delete" in main_menu_source,
    "Find BMS mismatch UI must expose Keep and Delete",
)
require(
    'makeModalButton("Keep Files"' in main_menu_source
    and 'makeModalButton("Delete Files"' in main_menu_source,
    "Find BMS mismatch actions must use the approved visible labels",
)
require(
    "findBmsDialogPolicy(findBmsJobRunning.load(), findBmsResult)"
    in main_menu_source,
    "Find BMS dismissal must use the tested dialog policy",
)

if failures:
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("Find BMS archive-flow audit passed")
