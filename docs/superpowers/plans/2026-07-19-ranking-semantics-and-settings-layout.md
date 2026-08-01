# Ranking Semantics and Settings Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a pinned, self-explanatory ranking header, expose every Bokutachi-supported judgment total and early/late pair in score details and new submissions, and restore Settings as the main-menu panel footer.

**Architecture:** Extend the canonical ranking and submission models first, then keep the Tachi parser/writer as thin JSON adapters over those models. The ranking header and row share column constants and a single compact-width function, while the score-detail overlay renders a fixed semantic grid. Settings remains outside the scrolling chart-action content so Yoga flex layout anchors it below the scroll view.

**Tech Stack:** C++20, nlohmann/json, custom Yoga-based view system, Catch2 tests, Python source-audit tests, CMake/CTest.

## Global Constraints

- Bokutachi exposes PGREAT, GREAT, GOOD, BAD, and POOR; do not invent a separate KPOOR judgment.
- KPOOR remains excluded from Tachi judgment counts and remains included in BP.
- Only complete, nonnegative early/late pairs are displayed or submitted.
- Do not rewrite frozen JSON in existing durable outbox rows.
- Preserve ranking pagination, virtualization, row selection, explicit modal dismissal, and comparison behavior.
- Settings must be a fixed footer below the flexing right-panel scroll view.

---

### Task 1: Parse and Present Complete Bokutachi Judgment Data

**Files:**
- Modify: `src/ir/IrRankingModels.h`
- Modify: `src/ir/tachi/TachiRankingParser.cpp`
- Modify: `src/ir/IrRankingModal.h`
- Modify: `src/ir/IrRankingModal.cpp`
- Test: `tests/tachi_ranking_parser_tests.cpp`
- Test: `tests/ir_ranking_modal_tests.cpp`

**Interfaces:**
- Consumes: native Bokutachi PB fields `scoreData.judgements.{pgreat,great,good,bad,poor}` and `scoreData.optional.{epg,lpg,egr,lgr,egd,lgd,ebd,lbd,epr,lpr}`.
- Produces: optional totals and timing pairs on `IrChartRankingEntry`, plus `total*Text`, `early*Text`, and `late*Text` fields on `IrRankingScoreDetailPresentation`.

- [ ] **Step 1: Extend parser fixtures with all supported totals and timing pairs**

Add a complete fixture whose judgment counts sum to 826 and whose EX score is 1284:

```cpp
"judgements": {
  "pgreat": 511, "great": 262, "good": 31, "bad": 14, "poor": 8
},
"optional": {
  "epg": 336, "lpg": 175, "egr": 201, "lgr": 61,
  "egd": 13, "lgd": 18, "ebd": 11, "lbd": 3,
  "epr": 0, "lpr": 8
}
```

Assert every value, and add cases proving a partial pair degrades only that pair while a negative or non-integer present value rejects the response.

- [ ] **Step 2: Extend modal-model tests for five semantic rows and partial availability**

Populate one entry with five totals and ten timing values, then assert:

```cpp
REQUIRE(detail->totalGoodText == "20");
REQUIRE(detail->earlyGoodText == "12");
REQUIRE(detail->lateGoodText == "8");
REQUIRE(detail->totalBadText == "6");
REQUIRE(detail->totalPoorText == "4");
REQUIRE(detail->judgementBreakdownAvailable);
```

Also assert missing cells render `"\xE2\x80\x94"` and totals alone make the judgment grid available.

- [ ] **Step 3: Run the focused tests and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target tachi_ranking_parser_tests ir_ranking_modal_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(tachi_ranking_parser_tests|ir_ranking_modal_tests)$'
```

Expected: compilation fails because the new model fields do not exist.

- [ ] **Step 4: Add canonical fields and parse all five rows**

Add optional integer totals and timing pairs:

```cpp
std::optional<int> pGreat;
std::optional<int> great;
std::optional<int> good;
std::optional<int> bad;
std::optional<int> poor;
std::optional<int> earlyGood;
std::optional<int> lateGood;
std::optional<int> earlyBad;
std::optional<int> lateBad;
std::optional<int> earlyPoor;
std::optional<int> latePoor;
```

Parse optional nonnegative integers, clear both sides when only one side is present, and clear a pair when its sum contradicts an available total. Preserve the existing PGREAT/GREAT EX-score validation when aggregate totals are absent.

- [ ] **Step 5: Build the complete detail presentation**

Use `integerOrMissing` for all totals and pairs. Set `judgementBreakdownAvailable` when any supported total exists or any complete pair exists, instead of requiring all four legacy PGREAT/GREAT timing cells.

- [ ] **Step 6: Run the focused tests and verify GREEN**

Run the Task 1 command again. Expected: both tests pass.

- [ ] **Step 7: Commit the data-flow slice**

```bash
git add src/ir/IrRankingModels.h src/ir/tachi/TachiRankingParser.cpp src/ir/IrRankingModal.h src/ir/IrRankingModal.cpp tests/tachi_ranking_parser_tests.cpp tests/ir_ranking_modal_tests.cpp
git commit -m "feat: expose complete Bokutachi judgment details"
```

### Task 2: Submit Every Supported Timing Pair

**Files:**
- Modify: `src/ir/IrSubmission.h`
- Modify: `src/ir/IrSubmission.cpp`
- Modify: `src/ir/tachi/TachiBatchManual.cpp`
- Test: `tests/ir_driver_tests.cpp`
- Test: `tests/tachi_batch_manual_tests.cpp`

**Interfaces:**
- Consumes: authentic replay events with `Press`, `Release`, or `Miss` actions and aggregate score judgments.
- Produces: `earlyGood`, `lateGood`, `earlyBad`, `lateBad`, `earlyPoor`, and `latePoor` in `IrSubmission`, serialized as `egd`, `lgd`, `ebd`, `lbd`, `epr`, and `lpr`.

- [ ] **Step 1: Add replay reconstruction and payload expectations**

Extend an IR driver fixture with early and late GOOD, BAD, and POOR events. Assert the canonical submission contains the six new values. Extend `validSubmission()` and payload assertions:

```cpp
REQUIRE(optional.at("egd") == submission.earlyGood);
REQUIRE(optional.at("lgd") == submission.lateGood);
REQUIRE(optional.at("ebd") == submission.earlyBad);
REQUIRE(optional.at("lbd") == submission.lateBad);
REQUIRE(optional.at("epr") == submission.earlyPoor);
REQUIRE(optional.at("lpr") == submission.latePoor);
```

Add an inconsistent lower pair case and require Batch Manual validation to reject it before transport.

- [ ] **Step 2: Run the submission tests and verify RED**

```bash
cmake --build cmake-build-debug --target ir_driver_tests tachi_batch_manual_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(ir_driver_tests|tachi_batch_manual_tests)$'
```

Expected: compilation fails on the six missing submission fields.

- [ ] **Step 3: Reconstruct complete timing pairs**

Count only authentic `Press`, `Release`, and `Miss` judgment events. Continue using score-delta checks for PGREAT and GREAT so classic-LN duplicate heads are excluded. Ignore `Gauge` and `Mine`. Set `hasJudgementBreakdown` only when all five early/late sums equal their aggregate counts.

- [ ] **Step 4: Validate and emit all ten metrics**

When `hasJudgementBreakdown` is true, reject negative fields or any pair whose sum differs from its aggregate. Emit the existing four fields and the new six fields into `scoreData.optional`. Do not change `scoreData.judgements.poor` or BP construction.

- [ ] **Step 5: Run the submission tests and verify GREEN**

Run the Task 2 command again. Expected: both tests pass.

- [ ] **Step 6: Commit the submission slice**

```bash
git add src/ir/IrSubmission.h src/ir/IrSubmission.cpp src/ir/tachi/TachiBatchManual.cpp tests/ir_driver_tests.cpp tests/tachi_batch_manual_tests.cpp
git commit -m "feat: submit complete Bokutachi timing breakdown"
```

### Task 3: Add a Pinned, Aligned Ranking Header

**Files:**
- Modify: `src/view/RecyclerView.h`
- Modify: `src/ir/IrRankingModal.h`
- Modify: `src/ir/IrRankingModal.cpp`
- Modify: `src/ir/IrRankingModalView.cpp`
- Modify: `scripts/check_ir_ranking_detail_flow.py`

**Interfaces:**
- Consumes: `RecyclerView::getVisibleItemWidth()` and `useCompactIrRankingColumns(int width)`.
- Produces: a header aligned with `RankingRowView`, showing eight labels in wide mode and four retained labels in compact mode.

- [ ] **Step 1: Strengthen the ranking source audit**

Require the eight labels and shared alignment hooks:

```python
for label in ("Rank", "Player", "EX Score", "EX Rate", "Lamp", "BP", "Max Combo", "Achieved"):
    require(label in view_source, f"missing ranking header label: {label}")
require("getVisibleItemWidth()" in view_source, "header must use recycler item width")
require("useCompactIrRankingColumns" in view_source, "header and rows must share compact policy")
```

- [ ] **Step 2: Run the audit and verify RED**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R '^ir_ranking_detail_flow_audit$'
```

Expected: failure reporting missing header labels or alignment hooks.

- [ ] **Step 3: Expose the recycler's effective item width**

Add a read-only public accessor that delegates to the existing gutter-aware private calculation:

```cpp
[[nodiscard]] int getVisibleItemWidth() const { return visibleItemWidth(); }
```

- [ ] **Step 4: Share column widths and compact policy**

Replace row-local numbers with named constants for Rank 58, EX Score 152, EX Rate 86, Lamp 174/144, BP 62, Max Combo 88, and Achieved 172. Export and use one `useCompactIrRankingColumns(int width) noexcept` predicate for both rows and header.

- [ ] **Step 5: Render the pinned header**

Create a `RankingTableHeaderView` above the recycler in a column container. Bind its width from `list->getVisibleItemWidth()` after layout and refresh. Hide it for loading, empty, and error states; in success state show the wide or compact labels that match the row columns.

- [ ] **Step 6: Run the audit and view/model tests**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R '^(ir_ranking_detail_flow_audit|ir_ranking_modal_tests|view_layout_tests)$'
```

Expected: all selected tests pass.

- [ ] **Step 7: Commit the ranking-header slice**

```bash
git add src/view/RecyclerView.h src/ir/IrRankingModal.h src/ir/IrRankingModal.cpp src/ir/IrRankingModalView.cpp scripts/check_ir_ranking_detail_flow.py
git commit -m "feat: label Bokutachi ranking columns"
```

### Task 4: Replace Judgment Cards with a Semantic Grid

**Files:**
- Modify: `src/ir/IrRankingModalView.cpp`
- Modify: `scripts/check_ir_ranking_detail_flow.py`

**Interfaces:**
- Consumes: all fifteen judgment text fields and `judgementBreakdownAvailable` from `IrRankingScoreDetailPresentation`.
- Produces: five fixed rows with Total, Early, and Late columns and a KPOOR capability note.

- [ ] **Step 1: Add semantic-grid audit requirements**

Require each judgment label, the `Total`/`Early`/`Late` column labels, all lower timing presentation fields, and this capability copy:

```text
KPOOR is not exposed separately by Bokutachi; BP remains aggregate.
```

- [ ] **Step 2: Run the audit and verify RED**

Run `ctest --test-dir cmake-build-debug --output-on-failure -R '^ir_ranking_detail_flow_audit$'`. Expected: failure on the missing semantic grid.

- [ ] **Step 3: Build and bind the five-row grid**

Replace the horizontal four-card row with a header and five rows. Use the result-screen judgment colors for row labels, `ui_theme::fastFeedback` for Early values, and `ui_theme::slowFeedback` for Late values. Store the Total/Early/Late cells in five-element arrays so `showScoreDetail()` binds the rows without duplicated control flow.

- [ ] **Step 4: Preserve partial and unavailable behavior**

Show the grid when `judgementBreakdownAvailable` is true, leaving missing cells as em dashes. Show the existing unavailable panel only when every supported total and timing pair is absent. Place the KPOOR/BP note beneath the grid and increase the safe-area-capped detail geometry enough to fit all five rows.

- [ ] **Step 5: Run audit and modal tests**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R '^(ir_ranking_detail_flow_audit|ir_ranking_modal_tests)$'
```

Expected: all selected tests pass.

- [ ] **Step 6: Commit the semantic-grid slice**

```bash
git add src/ir/IrRankingModalView.cpp scripts/check_ir_ranking_detail_flow.py
git commit -m "feat: render semantic Bokutachi judgment grid"
```

### Task 5: Restore Settings as the Right-Panel Footer

**Files:**
- Create: `scripts/check_main_menu_settings_anchor.py`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: the existing `right`, `rightScroll`, `rightContent`, and `settingsButton` views.
- Produces: a source-audited hierarchy where `rightContent` belongs to the scroll view and Settings is added directly to `right` afterward.

- [ ] **Step 1: Add a failing hierarchy audit**

Create a Python audit that locates the main-menu construction block and asserts ordering and ownership:

```python
require("rightContent->addView(settingsButton)" not in block,
        "Settings must not scroll with chart actions")
require_in_order(block,
                 "rightScroll->setContentView(rightContent);",
                 "right->addView(rightScroll);",
                 "right->addView(settingsButton);")
```

Register it as `main_menu_settings_anchor_audit` beside the existing main-menu ranking audit.

- [ ] **Step 2: Run the audit and verify RED**

```bash
cmake -S . -B cmake-build-debug
ctest --test-dir cmake-build-debug --output-on-failure -R '^main_menu_settings_anchor_audit$'
```

Expected: failure because Settings is currently in `rightContent`.

- [ ] **Step 3: Restore footer ownership**

Delete `settingsSpacer`, remove Settings from `rightContent`, give `right` a 12-pixel gap and 16-pixel bottom padding, and add views in this order:

```cpp
rightScroll->setContentView(rightContent);
right->addView(rightScroll);
right->addView(settingsButton);
```

Keep `rightScroll->setFlex(1)` so it consumes the space above the footer.

- [ ] **Step 4: Run main-menu audits and view tests**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R '^(main_menu_settings_anchor_audit|main_menu_ranking_flow_audit|view_layout_tests)$'
```

Expected: all selected tests pass.

- [ ] **Step 5: Commit the footer fix**

```bash
git add scripts/check_main_menu_settings_anchor.py src/scene/MainMenuScene.cpp CMakeLists.txt
git commit -m "fix: anchor Settings below main menu actions"
```

### Task 6: Integrate, Verify, and Publish

**Files:**
- Modify only files found by the verification/review steps if required.

**Interfaces:**
- Consumes: all five completed slices.
- Produces: a buildable, tested `feature/bokutachi-ir` branch pushed to its existing remote PR.

- [ ] **Step 1: Review the complete diff**

```bash
git status --short
git diff --check
git diff origin/feature/bokutachi-ir...HEAD --stat
git diff origin/feature/bokutachi-ir...HEAD
```

Expected: only intended ranking, submission, settings, tests, audits, and documentation changes; no whitespace errors or credentials.

- [ ] **Step 2: Run focused regression tests**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R '^(tachi_ranking_parser_tests|ir_ranking_modal_tests|ir_driver_tests|tachi_batch_manual_tests|ir_ranking_detail_flow_audit|main_menu_settings_anchor_audit|main_menu_ranking_flow_audit|view_layout_tests)$'
```

Expected: all selected tests pass.

- [ ] **Step 3: Build the desktop app**

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: target `main` builds successfully.

- [ ] **Step 4: Run the complete configured suite**

```bash
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: 100% tests passed.

- [ ] **Step 5: Commit any verification-only corrections**

If review or verification required a correction, stage only those files and commit with a message describing the correction. If the tree is clean, do not create an empty commit.

- [ ] **Step 6: Push the branch**

```bash
git push origin feature/bokutachi-ir
```

Expected: the remote branch advances to the verified local HEAD and updates the existing pull request.
