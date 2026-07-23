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
model_path = root / "src/ir/IrRankingModal.cpp"
model_source = model_path.read_text(encoding="utf-8") if model_path.is_file() else ""
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
require(
    "bool eventPoint(" not in source
    and "panel_->getX()" not in source
    and "panel_->getY()" not in source,
    "ranking modals must not dismiss from outside pointer hit testing",
)
require(
    source.count("new ModalScrim([this]()") == 2,
    "both ranking scrims must use explicit-only dismissal callbacks",
)
require(
    '#include "../view/IconText.h"' in source
    and "constexpr uint32_t kIconXmark = 0xf00d;" in source
    and source.count("makeIconActionButton(kIconXmark,") == 2
    and 'makeActionButton("Close"' not in source,
    "both ranking modal headers must use Font Awesome xmark buttons",
)
require(
    "class RankingTableHeaderView" in source
    and 'makeHeaderLabel("Rank"' in source
    and 'makeHeaderLabel("Player"' in source
    and 'makeHeaderLabel("EX Score"' in source
    and 'makeHeaderLabel("EX Rate"' in source
    and 'makeHeaderLabel("Lamp"' in source
    and 'makeHeaderLabel("BP"' in source
    and 'makeHeaderLabel("Max Combo"' in source
    and 'makeHeaderLabel("Achieved"' in source,
    "ranking list must have a pinned header for every visible column",
)
require(
    "getVisibleItemWidth()" in source
    and source.count("useCompactIrRankingColumns")
    + model_source.count("useCompactIrRankingColumns")
    >= 2,
    "ranking header and rows must share recycler width and compact policy",
)
require(
    'makeJudgementRow("PGREAT"' in source
    and 'makeJudgementRow("GREAT"' in source
    and 'makeJudgementRow("GOOD"' in source
    and 'makeJudgementRow("BAD"' in source
    and 'makeJudgementRow("POOR"' in source
    and 'setText("Total")' in source
    and 'setText("Early")' in source
    and 'setText("Late")' in source,
    "score details must arrange every supported judgment in semantic rows",
)
require(
    "detail.totalGoodText" in source
    and "detail.earlyGoodText" in source
    and "detail.lateGoodText" in source
    and "detail.totalBadText" in source
    and "detail.earlyBadText" in source
    and "detail.lateBadText" in source
    and "detail.totalPoorText" in source
    and "detail.earlyPoorText" in source
    and "detail.latePoorText" in source,
    "score detail view must bind all Bokutachi judgment cells",
)
require(
    "applyScoreDetailJudgementColumnLayout();" in source
    and "layoutIrRankingJudgementColumns(" in source
    and source.count("TextView::RIGHT") >= 4,
    "judgment headings and counts must use one shared, right-aligned column layout",
)
require(
    "KPOOR is not exposed separately by Bokutachi; BP remains aggregate."
    in source,
    "score details must explain the Bokutachi KPOOR capability boundary",
)

if failures:
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("IR ranking score-detail flow audit passed")
