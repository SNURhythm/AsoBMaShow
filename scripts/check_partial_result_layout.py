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


def braced_block(source: str, marker: str) -> str:
    start = source.find(marker)
    if start < 0:
        failures.append(f"missing authoritative result path marker: {marker}")
        return ""
    opening = source.find("{", start)
    if opening < 0:
        failures.append(f"missing opening brace after: {marker}")
        return ""
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    failures.append(f"unterminated authoritative result path: {marker}")
    return ""


skin_types = read("src/skin/SkinTypes.h")
default_skin = read("src/skin/DefaultSkin.cpp")

require(
    re.search(
        r"const\s+ResultPresentationModel\s*\*\s*presentation\s*=\s*nullptr\s*;",
        skin_types,
    )
    is not None,
    "ResultSkinData must append an optional ResultPresentationModel pointer",
)

override = braced_block(
    default_skin, "if (data != nullptr && data->presentation != nullptr)"
)
renderer = braced_block(
    default_skin, "void DefaultSkin::buildPresentationResultLayout("
)
authoritative = override + "\n" + renderer

require(
    "buildPresentationResultLayout" in override
    and "*data->presentation" in override,
    "the non-null presentation pointer must be authoritative",
)

for pattern, message in (
    (
        r"\.value_or\s*\(\s*0(?:\.0)?[fFuUlL]*\s*\)",
        "authoritative presentation path must not collapse absence with value_or(0)",
    ),
    (
        r"\bRhythmState\b",
        "authoritative presentation path must not fabricate or inspect RhythmState",
    ),
    (
        r"\bReplayData\b",
        "authoritative presentation path must not fabricate or inspect ReplayData",
    ),
    (
        r"\bScoreProvenance\b",
        "authoritative presentation path must not fabricate or inspect ScoreProvenance",
    ),
    (
        r"\b(?:ChartMeta|ReplayEvent|ReplayLaneCoverEvent)\b",
        "authoritative presentation path must not fabricate charts or timing events",
    ),
    (
        r'"--"',
        "authoritative presentation path must not synthesize placeholder dashes",
    ),
):
    require(re.search(pattern, authoritative) is None, message)

require(
    "data->state" not in authoritative and "data->meta" not in authoritative,
    "authoritative presentation path must not dereference legacy state or metadata",
)

if failures:
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("partial result layout audit passed")
