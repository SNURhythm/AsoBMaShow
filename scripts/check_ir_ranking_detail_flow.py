#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys


root = (
    pathlib.Path(sys.argv[1])
    if len(sys.argv) > 1
    else pathlib.Path(__file__).parent.parent
).resolve()
source_path = root / "src/ir/IrRankingModalView.cpp"
source = source_path.read_text(encoding="utf-8") if source_path.is_file() else ""
failures: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


require(
    "showScoreDetails(index);" in source,
    "ranking row selection must open score details",
)
require(
    "portal.present(scoreDetailRoot);" in source,
    "score details must render above the ranking modal",
)
require(
    "portal.dismiss(scoreDetailRoot);" in source,
    "score details must have an explicit dismiss path",
)
require(
    source.count("hideScoreDetails();") >= 3,
    "refresh and parent close paths must dismiss score details",
)

if failures:
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("IR ranking score-detail flow audit passed")
