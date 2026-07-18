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
