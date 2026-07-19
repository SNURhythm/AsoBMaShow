#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys


root = (
    pathlib.Path(sys.argv[1])
    if len(sys.argv) > 1
    else pathlib.Path(__file__).parent.parent
).resolve()
source_path = root / "src/scene/MainMenuScene.cpp"

if not source_path.is_file():
    print("FAIL: missing src/scene/MainMenuScene.cpp", file=sys.stderr)
    raise SystemExit(1)

source = source_path.read_text(encoding="utf-8")
ranking_open_start = source.find("void MainMenuScene::openRankingsForSelection()")
ranking_open_end = source.find(
    "MainMenuScene::EffectivePlayOptionSelection", ranking_open_start
)
if ranking_open_start < 0 or ranking_open_end < 0:
    print("FAIL: unable to locate the Rankings open flow", file=sys.stderr)
    raise SystemExit(1)

ranking_open = source[ranking_open_start:ranking_open_end]
if "LoadBestScoreForRuleset(" not in ranking_open or (
    "RulesetDescriptor::For(GameplayRuleset::LR2)" not in ranking_open
):
    print(
        "FAIL: Bokutachi local comparison must select an LR2-specific PB",
        file=sys.stderr,
    )
    raise SystemExit(1)

selection_start = source.find("recyclerView->onSelected =")
selection_end = source.find("recyclerView->onUnselected =", selection_start)
if selection_start < 0 or selection_end < 0:
    print("FAIL: unable to locate the main-menu chart selection handler", file=sys.stderr)
    raise SystemExit(1)

selection_handler = source[selection_start:selection_end]
assignment = selection_handler.find("selectedChartRecord = item;")
ranking_refresh = selection_handler.find("refreshRankingsButton();")
if assignment < 0 or ranking_refresh < assignment:
    print(
        "FAIL: selecting a chart must refresh Rankings after storing the selection",
        file=sys.stderr,
    )
    raise SystemExit(1)

print("main-menu ranking flow audit passed")
