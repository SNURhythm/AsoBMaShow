# Ranking Score Details Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every Bokutachi ranking row open a nested, responsive score-detail overlay using the judgement and metadata already returned by the ranking request.

**Architecture:** Preserve Tachi's validated early/late PGREAT and GREAT counts in `IrChartRankingEntry`, format an immutable detail presentation in `IrRankingModalModel`, and let `IrRankingModalView` own the nested overlay lifecycle. Reuse the existing `OverlayPortal`, `ModalScrim`, safe-area geometry, theme, and fixed-height virtualized list; do not make another network request.

**Tech Stack:** C++23, nlohmann/json, Yoga View hierarchy, `OverlayPortal`, `RecyclerView`, CTest, Python source audits.

## Global Constraints

- Work on `feature/bokutachi-ir`; do not push, merge, or deploy.
- Bokutachi remains the only active ranking provider.
- Row selection must behave the same on compact and wide layouts.
- Detail display must use the current ranking snapshot and make no HTTP request.
- Preserve list virtualization, fixed row heights, and scroll position.
- Missing optional BP, combo, or achievement time must display an em dash.
- Parent refresh, parent close, scene cleanup, and destruction must dismiss the detail overlay.
- Implement production behavior only after its focused regression test fails for the expected reason.

---

### Task 1: Preserve early/late judgement counts in normalized ranking entries

**Files:**
- Modify: `tests/tachi_ranking_parser_tests.cpp:25-90`
- Modify: `src/ir/IrRankingModels.h:25-39`
- Modify: `src/ir/tachi/TachiRankingParser.cpp:125-205`

**Interfaces:**
- Consumes: validated Tachi ranking row fields `epg`, `lpg`, `egr`, and `lgr`.
- Produces: `IrChartRankingEntry::earlyPGreat`, `latePGreat`, `earlyGreat`, and `lateGreat` as nonnegative `int` values.

- [ ] **Step 1: Add a failing normalization test**

In `testEmptyAndAuthenticatedRanking()`, set distinct timing values on the
authenticated row while retaining a valid 110 EX score:

```cpp
auto current = row("", 110, 5, 0);
current["epg"] = 31;
current["lpg"] = 19;
current["egr"] = 6;
current["lgr"] = 4;
current["maxcombo"] = nullptr;
```

After the existing score assertion, require:

```cpp
expect(entry.earlyPGreat == 31 && entry.latePGreat == 19 &&
           entry.earlyGreat == 6 && entry.lateGreat == 4,
       "early and late PGREAT/GREAT counts are retained");
```

- [ ] **Step 2: Run the parser test to verify RED**

```bash
cmake --build cmake-build-debug --target tachi_ranking_parser_tests -j 6
```

Expected: compilation fails because the four normalized entry fields do not
exist.

- [ ] **Step 3: Add provider-neutral fields and copy validated counts**

Add the fields after `maxScore` in `IrChartRankingEntry`:

```cpp
int earlyPGreat = 0;
int latePGreat = 0;
int earlyGreat = 0;
int lateGreat = 0;
```

In `parseRow()`, assign the already-range-checked JSON integers when creating
the normalized entry:

```cpp
.earlyPGreat = static_cast<int>(*epg),
.latePGreat = static_cast<int>(*lpg),
.earlyGreat = static_cast<int>(*egr),
.lateGreat = static_cast<int>(*lgr),
```

Do not retain the JSON row or change ordering/rank tuple behavior.

- [ ] **Step 4: Run the parser test to verify GREEN**

```bash
cmake --build cmake-build-debug --target tachi_ranking_parser_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^tachi_ranking_parser_tests$'
```

Expected: the target links and the parser test passes.

- [ ] **Step 5: Commit normalized judgement evidence**

```bash
git add src/ir/IrRankingModels.h src/ir/tachi/TachiRankingParser.cpp \
  tests/tachi_ranking_parser_tests.cpp
git commit -m "feat: retain ranking judgement details"
```

---

### Task 2: Format a complete score-detail presentation

**Files:**
- Modify: `tests/ir_ranking_modal_tests.cpp:35-235`
- Modify: `src/ir/IrRankingModal.h:28-108`
- Modify: `src/ir/IrRankingModal.cpp:282-325`

**Interfaces:**
- Consumes: one normalized `IrChartRankingEntry` from Task 1.
- Produces: `std::optional<IrRankingScoreDetailPresentation> IrRankingModalModel::scoreDetail(int index) const`.

- [ ] **Step 1: Enrich the ranking fixture and add failing detail tests**

Add distinct judgement counts to the first `ranking()` fixture entry:

```cpp
.earlyPGreat = 430,
.latePGreat = 470,
.earlyGreat = 48,
.lateGreat = 52,
```

Add this test:

```cpp
void testScoreDetailFormatsCompleteAndMissingData() {
  ir::IrRankingModalModel model;
  model.open(request(), "Test Chart");
  REQUIRE(model.apply(snapshot(ir::IrRankingSnapshotState::Succeeded)));

  const auto detail = model.scoreDetail(0);
  REQUIRE(detail.has_value());
  REQUIRE(detail->rankText == "#1");
  REQUIRE(detail->playerText == "AAA");
  REQUIRE(detail->scoreText == "1900 / 2000");
  REQUIRE(detail->rateText == "95.00%");
  REQUIRE(detail->lampText == "FULL COMBO");
  REQUIRE(detail->earlyPGreatText == "430");
  REQUIRE(detail->latePGreatText == "470");
  REQUIRE(detail->earlyGreatText == "48");
  REQUIRE(detail->lateGreatText == "52");
  REQUIRE(detail->badPointsText == "0");
  REQUIRE(detail->maxComboText == "1000");
  REQUIRE(detail->achievementTimeText != "\xE2\x80\x94");
  REQUIRE(detail->clearType == kClearTypeFullComboRank);
  REQUIRE(!detail->highlighted);

  const auto missing = model.scoreDetail(1);
  REQUIRE(missing.has_value());
  REQUIRE(missing->badPointsText == "\xE2\x80\x94");
  REQUIRE(missing->maxComboText == "\xE2\x80\x94");
  REQUIRE(missing->achievementTimeText == "\xE2\x80\x94");
  REQUIRE(missing->highlighted);

  REQUIRE(!model.scoreDetail(-1).has_value());
  REQUIRE(!model.scoreDetail(2).has_value());
}
```

Invoke it from `main()`.

- [ ] **Step 2: Build the modal-model test to verify RED**

```bash
cmake --build cmake-build-debug --target ir_ranking_modal_tests -j 6
```

Expected: compilation fails because `scoreDetail()` and
`IrRankingScoreDetailPresentation` do not exist.

- [ ] **Step 3: Add the score-detail presentation boundary**

Add this struct after `IrRankingRowPresentation` in `IrRankingModal.h`:

```cpp
struct IrRankingScoreDetailPresentation {
  std::string rankText;
  std::string playerText;
  std::string scoreText;
  std::string rateText;
  std::string lampText;
  std::string earlyPGreatText;
  std::string latePGreatText;
  std::string earlyGreatText;
  std::string lateGreatText;
  std::string badPointsText;
  std::string maxComboText;
  std::string achievementTimeText;
  int clearType = kClearTypeFailedRank;
  bool highlighted = false;
};
```

Declare on `IrRankingModalModel`:

```cpp
[[nodiscard]] std::optional<IrRankingScoreDetailPresentation>
scoreDetail(int index) const;
```

Implement it beside `row()`:

```cpp
std::optional<IrRankingScoreDetailPresentation>
IrRankingModalModel::scoreDetail(int index) const {
  if (!presentation_.ranking || index < 0 ||
      index >= static_cast<int>(presentation_.ranking->entries.size())) {
    return std::nullopt;
  }
  const auto &entry = presentation_.ranking->entries[index];
  return IrRankingScoreDetailPresentation{
      .rankText = rankText(entry.rank),
      .playerText = playerText(entry),
      .scoreText = scoreText(entry.score, entry.maxScore),
      .rateText = formatIrRankingRate(entry.score, entry.maxScore),
      .lampText = clearTypeRankToLabel(entry.clearType),
      .earlyPGreatText = std::to_string(entry.earlyPGreat),
      .latePGreatText = std::to_string(entry.latePGreat),
      .earlyGreatText = std::to_string(entry.earlyGreat),
      .lateGreatText = std::to_string(entry.lateGreat),
      .badPointsText = integerOrMissing(entry.badPoints),
      .maxComboText = integerOrMissing(entry.maxCombo),
      .achievementTimeText =
          formatIrRankingTimestamp(entry.achievedAtUnixMillis),
      .clearType = entry.clearType,
      .highlighted = entry.currentUser,
  };
}
```

- [ ] **Step 4: Run modal-model tests to verify GREEN**

```bash
cmake --build cmake-build-debug --target ir_ranking_modal_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^ir_ranking_modal_tests$'
```

Expected: the model target links and all modal-model tests pass.

- [ ] **Step 5: Commit the presentation boundary**

```bash
git add src/ir/IrRankingModal.h src/ir/IrRankingModal.cpp \
  tests/ir_ranking_modal_tests.cpp
git commit -m "feat: format ranking score details"
```

---

### Task 3: Open a nested detail overlay from every ranking row

**Files:**
- Create: `scripts/check_ir_ranking_detail_flow.py`
- Modify: `CMakeLists.txt:500-530`
- Modify: `tests/ir_ranking_modal_tests.cpp:190-235`
- Modify: `src/ir/IrRankingModal.h:28-108`
- Modify: `src/ir/IrRankingModal.cpp:155-325`
- Modify: `src/ir/IrRankingModalView.cpp:20-555`

**Interfaces:**
- Consumes: `IrRankingModalModel::scoreDetail(int)` from Task 2.
- Produces: `IrRankingModal::Impl::showScoreDetails(int)` and `hideScoreDetails()` with a caller-owned `scoreDetailRoot` presented through the existing `OverlayPortal`.

- [ ] **Step 1: Add a failing interaction audit**

Create `scripts/check_ir_ranking_detail_flow.py` with the repository audit
pattern used by the other scripts:

```python
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
```

Register it beside `main_menu_ranking_flow_audit` in `CMakeLists.txt`:

```cmake
add_test(NAME ir_ranking_detail_flow_audit
    COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/scripts/check_ir_ranking_detail_flow.py
            ${CMAKE_SOURCE_DIR})
set_tests_properties(ir_ranking_detail_flow_audit PROPERTIES
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
```

- [ ] **Step 2: Reconfigure and run the audit to verify RED**

```bash
cmake -S . -B cmake-build-debug
ctest --test-dir cmake-build-debug --output-on-failure -R '^ir_ranking_detail_flow_audit$'
```

Expected: FAIL with all four missing interaction contracts.

- [ ] **Step 3: Build the reusable detail surface**

In `IrRankingModal::Impl`, add caller-owned pointers for a second
`ModalScrim`, its panel, title, summary values, judgement values, metadata
values, lamp, and Close button, plus `bool scoreDetailOpen = false`:

```cpp
ModalScrim *scoreDetailRoot = nullptr;
View *scoreDetailPanel = nullptr;
TextView *scoreDetailTitle = nullptr;
TextView *scoreDetailScore = nullptr;
TextView *scoreDetailRate = nullptr;
TextView *scoreDetailLamp = nullptr;
TextView *scoreDetailEarlyPGreat = nullptr;
TextView *scoreDetailLatePGreat = nullptr;
TextView *scoreDetailEarlyGreat = nullptr;
TextView *scoreDetailLateGreat = nullptr;
TextView *scoreDetailBadPoints = nullptr;
TextView *scoreDetailMaxCombo = nullptr;
TextView *scoreDetailAchievementTime = nullptr;
bool scoreDetailOpen = false;
```

Use a small helper that creates a vertical metric card and exposes its value:

```cpp
View *makeMetricCard(std::string label, TextView *&value, int valueSize = 22) {
  auto *card = new View();
  card->setFlex(1.0F)->setMinWidth(0);
  card->setFlexDirection(FlexDirection::Column);
  card->setPadding(Edge::All, 10);
  card->setGap(4);
  card->setThemedBackgroundColor(ui_theme::panelSubtle);
  card->setCornerRadius(ui_theme::controlRadius());
  auto *caption = makeText(14);
  caption->setText(std::move(label));
  caption->setThemedColor(ui_theme::textMuted);
  value = makeText(valueSize);
  card->addView(caption);
  card->addView(value);
  return card;
}
```

Build these fixed-height rows after the helper:

```cpp
scoreDetailPanel = new View();
scoreDetailRoot = new ModalScrim(scoreDetailPanel,
                                 [this]() { hideScoreDetails(); });
scoreDetailRoot->setPositionType(YGPositionTypeAbsolute);
scoreDetailRoot->setPosition(Edge::Left, 0);
scoreDetailRoot->setPosition(Edge::Top, 0);
scoreDetailRoot->setFlexDirection(FlexDirection::Column);
scoreDetailRoot->setAlignItems(YGAlignCenter);
scoreDetailRoot->setJustifyContent(YGJustifyCenter);
scoreDetailRoot->setBackgroundColor(Color(2, 5, 9, 214));

scoreDetailPanel->setFlexDirection(FlexDirection::Column);
scoreDetailPanel->setAlignItems(YGAlignStretch);
scoreDetailPanel->setPadding(Edge::All, 18);
scoreDetailPanel->setGap(12);
scoreDetailPanel->setThemedBackgroundColor(ui_theme::panelStrong);
scoreDetailPanel->setCornerRadius(ui_theme::panelRadius());
scoreDetailPanel->setThemedBorderColor(ui_theme::hairlineStrong);
scoreDetailPanel->setBorderWidth(1);
scoreDetailPanel->setThemedShadow(ui_theme::shadow,
                                 ui_theme::kModalShadow);

auto *detailHeader = new View();
detailHeader->setFlexDirection(FlexDirection::Row);
detailHeader->setAlignItems(YGAlignCenter);
detailHeader->setGap(10)->setHeight(52)->setFlexShrink(0);
scoreDetailTitle = makeText(25);
scoreDetailTitle->setFlex(1);
detailHeader->addView(scoreDetailTitle);
detailHeader->addView(makeActionButton(
    "Close", 96, [this]() { hideScoreDetails(); }));

auto makeMetricRow = [] {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setAlignItems(YGAlignStretch);
  row->setGap(10)->setFlexShrink(0);
  return row;
};
auto *summary = makeMetricRow();
summary->setHeight(82);
summary->addView(makeMetricCard("EX Score", scoreDetailScore));
summary->addView(makeMetricCard("Rate", scoreDetailRate));
summary->addView(makeMetricCard("Lamp", scoreDetailLamp, 17));

auto *judgements = makeMetricRow();
judgements->setHeight(88);
judgements->addView(
    makeMetricCard("PGREAT Early", scoreDetailEarlyPGreat));
judgements->addView(
    makeMetricCard("PGREAT Late", scoreDetailLatePGreat));
judgements->addView(makeMetricCard("GREAT Early", scoreDetailEarlyGreat));
judgements->addView(makeMetricCard("GREAT Late", scoreDetailLateGreat));

auto *metadata = makeMetricRow();
metadata->setHeight(82);
metadata->addView(makeMetricCard("BP", scoreDetailBadPoints));
metadata->addView(makeMetricCard("Max Combo", scoreDetailMaxCombo));
metadata->addView(
    makeMetricCard("Achieved", scoreDetailAchievementTime, 16));

scoreDetailPanel->addView(detailHeader);
scoreDetailPanel->addView(summary);
scoreDetailPanel->addView(judgements);
scoreDetailPanel->addView(metadata);
scoreDetailRoot->addView(scoreDetailPanel);
scoreDetailRoot->setVisible(false);
```

The maximum detail width is `760`, maximum height is `480`, and the stronger
scrim keeps the parent modal visibly behind it.

- [ ] **Step 4: Bind and present score details**

Add:

```cpp
void showScoreDetails(int index) {
  const auto detailValue = model.scoreDetail(index);
  if (!detailValue) {
    return;
  }
  hideScoreDetails();
  const auto &detail = *detailValue;
  scoreDetailTitle->setText(detail.rankText + "   " + detail.playerText);
  scoreDetailScore->setText(detail.scoreText);
  scoreDetailRate->setText(detail.rateText);
  scoreDetailLamp->setText(detail.lampText);
  scoreDetailEarlyPGreat->setText(detail.earlyPGreatText);
  scoreDetailLatePGreat->setText(detail.latePGreatText);
  scoreDetailEarlyGreat->setText(detail.earlyGreatText);
  scoreDetailLateGreat->setText(detail.lateGreatText);
  scoreDetailBadPoints->setText(detail.badPointsText);
  scoreDetailMaxCombo->setText(detail.maxComboText);
  scoreDetailAchievementTime->setText(detail.achievementTimeText);
  const Color lampColor = clearLampColorForRank(detail.clearType);
  scoreDetailLamp->setBackgroundColor(lampColor);
  scoreDetailLamp->setColor(ui_theme::sdl(ui_theme::textOn(lampColor)));
  scoreDetailTitle->setThemedColor(detail.highlighted ? ui_theme::cyan
                                                       : ui_theme::textPrimary);
  scoreDetailOpen = true;
  scoreDetailRoot->setVisible(true);
  updateScoreDetailLayout(safeInsets());
  portal.present(scoreDetailRoot);
}

void hideScoreDetails() {
  portal.dismiss(scoreDetailRoot);
  scoreDetailRoot->setVisible(false);
  scoreDetailOpen = false;
}
```

Change list selection to:

```cpp
list->onSelected = [this](const auto &, int index) {
  showScoreDetails(index);
};
```

Delete `RankingRowView::detail_` and its compact expansion binding.

- [ ] **Step 5: Remove compact-only expansion state**

Delete `IrRankingModalModel::toggleExpanded()`, `expandedIndex_`,
`IrRankingRowPresentation::detailText`, `expanded`, and the compact expansion
conditions. In `row()`, use:

```cpp
const bool showDetails = !compact;
```

Replace `testResponsiveRowsKeepCoreFieldsAndExpandCompactDetails()` with
`testResponsiveRowsKeepFixedHeightCoreFields()`:

```cpp
void testResponsiveRowsKeepFixedHeightCoreFields() {
  ir::IrRankingModalModel model;
  model.open(request(), "Test Chart");
  REQUIRE(model.apply(snapshot(ir::IrRankingSnapshotState::Succeeded)));

  const auto wide = model.row(0, 900);
  REQUIRE(!wide.compact);
  REQUIRE(wide.showBadPoints);
  REQUIRE(wide.showMaxCombo);
  REQUIRE(wide.showAchievementTime);

  const auto compact = model.row(0, 560);
  REQUIRE(compact.compact);
  REQUIRE(!compact.showBadPoints);
  REQUIRE(!compact.showMaxCombo);
  REQUIRE(!compact.showAchievementTime);
  REQUIRE(!compact.rankText.empty());
  REQUIRE(!compact.playerText.empty());
  REQUIRE(!compact.rateText.empty());
  REQUIRE(!compact.lampText.empty());
}
```

Update the invocation in `main()`.

- [ ] **Step 6: Complete layout, input, and lifecycle handling**

Implement the layout function as:

```cpp
void updateScoreDetailLayout(const SafeInsets &safe) {
  const auto geometry =
      layoutIrRankingPanel({.viewportWidth = rendering::window_width,
                            .viewportHeight = rendering::window_height,
                            .safeTop = safe.top,
                            .safeLeft = safe.left,
                            .safeBottom = safe.bottom,
                            .safeRight = safe.right,
                            .margin = 36,
                            .maximumWidth = 760,
                            .maximumHeight = 480});
  scoreDetailRoot->setSize(rendering::window_width,
                           rendering::window_height);
  scoreDetailRoot->setPadding(Edge::Top, safe.top + 36);
  scoreDetailRoot->setPadding(Edge::Bottom, safe.bottom + 36);
  scoreDetailRoot->setPadding(Edge::Left, safe.left + 36);
  scoreDetailRoot->setPadding(Edge::Right, safe.right + 36);
  scoreDetailPanel->setWidth(static_cast<float>(geometry.width));
  scoreDetailPanel->setHeight(static_cast<float>(geometry.height));
  scoreDetailRoot->applyYogaLayout();
}
```

Call `hideScoreDetails()` before `service.refresh()` in `refresh()`, before
dismissing the parent root in `closeNow()`, and from destruction through
`closeNow()`. The destructor becomes:

```cpp
~Impl() {
  closeNow();
  delete root;
  delete scoreDetailRoot;
}
```

The nested `ModalScrim` handles Escape/Android Back/outside press and consumes
the event before the underlying rankings root.

- [ ] **Step 7: Run focused tests, the audit, and desktop build to verify GREEN**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R '^ir_ranking_detail_flow_audit$'
cmake --build cmake-build-debug --target ir_ranking_modal_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^ir_ranking_modal_tests$'
cmake --build cmake-build-debug --target main -j 6
```

Expected: the audit and modal-model tests pass and the desktop executable
links.

- [ ] **Step 8: Commit the interaction**

```bash
git add CMakeLists.txt scripts/check_ir_ranking_detail_flow.py \
  src/ir/IrRankingModal.h src/ir/IrRankingModal.cpp \
  src/ir/IrRankingModalView.cpp tests/ir_ranking_modal_tests.cpp
git commit -m "feat: open ranking score details"
```

---

### Task 4: Integrated verification

**Files:**
- Verify only; modify implementation or tests only when a failure identifies a defect in Tasks 1–3.

**Interfaces:**
- Consumes: normalized detail evidence, detail presentation, and nested overlay interaction.
- Produces: fresh build/test and source-hygiene evidence for handoff.

- [ ] **Step 1: Run focused builds and tests**

```bash
git diff --check HEAD~3..HEAD
cmake --build cmake-build-debug --target \
  tachi_ranking_parser_tests ir_ranking_modal_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R \
  '^(tachi_ranking_parser_tests|ir_ranking_modal_tests|ir_ranking_detail_flow_audit)$'
```

Expected: no whitespace errors, all targets link, and all three focused tests
pass.

- [ ] **Step 2: Run the complete suite**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

Expected: 100% pass with zero failures.

- [ ] **Step 3: Audit scope and branch state**

```bash
rg -n 'apiKey|Authorization|Bearer' src/ir/IrRankingModels.h \
  src/ir/IrRankingModal.h src/ir/IrRankingModal.cpp \
  src/ir/IrRankingModalView.cpp src/ir/tachi/TachiRankingParser.cpp
git status --short --branch
git log -4 --oneline
```

Expected: no credential reference in ranking data or presentation code; the
worktree is clean; the latest commits are the plan plus the three task commits.

- [ ] **Step 4: Report without pushing or deploying**

Report normalized early/late judgement preservation, consistent row-click
behavior, nested modal lifecycle, exact test results, commit IDs, and that no
push or deployment occurred.
