#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys


root = (
    pathlib.Path(sys.argv[1])
    if len(sys.argv) > 1
    else pathlib.Path(__file__).parent.parent
).resolve()
failures: list[str] = []


def read(relative: str) -> str:
    path = root / relative
    if not path.is_file():
        failures.append(f"missing required file: {relative}")
        return ""
    return path.read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


exporter = read("src/ResultImageExporter.cpp")

require(
    '#include "scene/ResultLayoutGeometry.h"' in exporter,
    "result photo exporter must consume the shared result layout geometry",
)
require(
    "result_layout::photoCanvasPixelHeight" in exporter,
    "result photo exporter must size its canvas from the photo grid",
)
require(
    "resultSkinData.showResultGraph = !analyticsModel.has_value()" in exporter,
    "analytics photos must replace the skin's stacked full-width graph",
)
require(
    "result_layout::photoVisualOrder()" in exporter,
    "result photo exporter must use the tested visual order",
)
for name in (
    'setName("resultPhotoVisuals")',
    'setName("resultPhotoPrimaryRow")',
    'setName("resultPhotoSecondaryRow")',
):
    require(name in exporter, f"result photo exporter is missing {name}")
require(
    "kPhotoAnalyticsExtraHeight" not in exporter,
    "result photo exporter must not retain stacked analytics extra height",
)

if failures:
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("result visual layout audit passed")
