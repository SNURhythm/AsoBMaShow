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
failures: list[str] = []


def function_body(signature: str, next_signature: str) -> str:
    start = source.find(signature)
    end = source.find(next_signature, start + len(signature))
    if start < 0 or end < 0:
        failures.append(f"unable to locate {signature}")
        return ""
    return source[start:end]


pause = function_body(
    "void MainMenuScene::onPause()", "void MainMenuScene::onResume()"
)
reload_list = function_body(
    "void MainMenuScene::reloadChartList(bool preserveViewState)",
    "std::optional<std::string> MainMenuScene::reloadScoreClearRanks()",
)

if "chartListCache.clear();" in pause:
    failures.append(
        "onPause must not invalidate the backing cache while RecyclerView keeps "
        "its external item provider"
    )
if "chartListCache.releasePages();" not in pause:
    failures.append("onPause must release only reloadable lazy chart pages")

required_reload_steps = [
    "projectedChartMetadataCache.recordsFor(",
    "projectedScoreQueryIndices(",
    "chartListCache.resetReferenced(",
]
position = -1
for step in required_reload_steps:
    position = reload_list.find(step, position + 1)
    if position < 0:
        failures.append(f"missing projected chart-list cache step: {step}")
        break

if "chartSession->QueryChartMeta(baseQuery, projectedRecords);" in reload_list:
    failures.append(
        "each projected lamp selection must not synchronously reload all base "
        "metadata"
    )

if failures:
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("main-menu chart-list lifecycle audit passed")
