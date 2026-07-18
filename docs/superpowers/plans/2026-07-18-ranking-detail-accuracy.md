# Ranking Detail Accuracy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Bokutachi ranking details truthful and make local BP use `BAD + POOR + KPOOR`.

**Architecture:** Keep the existing ranking endpoint and use its compatibility judgement fields only to calculate EX score. Represent authoritative early/late counts as optional normalized evidence, present an unavailable explanation when evidence is absent, and load BP from the selected local score row without changing the score-summary schema.

**Tech Stack:** C++23, nlohmann/json, SQLite, Yoga View hierarchy, CMake, CTest.

## Global Constraints

- Work on `feature/bokutachi-ir`; do not push, merge, deploy, or upload.
- Bokutachi remains the only active ranking provider.
- Never present Tachi's synthesized Beatoraja compatibility fields as real judgements.
- BP means exactly `BAD + POOR + KPOOR`.
- Result-screen `BREAK` remains exactly `BAD + POOR` and must not be changed.
- Do not change the IR outbox schema or store credentials in result/ranking data.
- Write and observe each focused regression failing before production edits.

---

### Task 1: Normalize only authoritative ranking judgements

**Files:**
- Modify: `tests/tachi_ranking_parser_tests.cpp`
- Modify: `tests/ir_ranking_modal_tests.cpp`
- Modify: `src/ir/IrRankingModels.h`
- Modify: `src/ir/tachi/TachiRankingParser.cpp`
- Modify: `src/ir/IrRankingModal.h`
- Modify: `src/ir/IrRankingModal.cpp`
- Modify: `src/ir/IrRankingModalView.cpp`

**Interfaces:**
- Consumes: validated `epg/lpg/egr/lgr` compatibility integers.
- Produces: optional `IrChartRankingEntry` timing values and `IrRankingScoreDetailPresentation::judgementBreakdownAvailable`.

- [ ] **Step 1: Write failing parser and modal tests**

Change the parser fixture with distinct `epg/lpg/egr/lgr` values and require
that score remains correct while all four normalized timing values are absent.
In the modal test, retain one manually constructed entry with all four optional
values and require the missing entry to return em dashes plus
`judgementBreakdownAvailable == false`.

- [ ] **Step 2: Run the focused tests and verify RED**

```bash
cmake --build cmake-build-debug --target tachi_ranking_parser_tests ir_ranking_modal_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(tachi_ranking_parser_tests|ir_ranking_modal_tests)$'
```

Expected: compilation or assertions fail because normalized judgement fields
are non-optional and missing detail evidence is currently rendered as zero.

- [ ] **Step 3: Implement optional evidence and unavailable presentation**

Use these normalized fields:

```cpp
std::optional<int> earlyPGreat;
std::optional<int> latePGreat;
std::optional<int> earlyGreat;
std::optional<int> lateGreat;
```

Do not assign them in `TachiRankingParser`; continue using the validated JSON
integers locally to calculate `score`. Format them with `integerOrMissing` and
set availability only when all four optionals contain values:

```cpp
const bool judgementBreakdownAvailable =
    entry.earlyPGreat && entry.latePGreat &&
    entry.earlyGreat && entry.lateGreat;
```

In the view, toggle the existing four-card row against an 88-point centered
message: `Judgement breakdown unavailable from Bokutachi rankings.`

- [ ] **Step 4: Run the focused tests and verify GREEN**

```bash
cmake --build cmake-build-debug --target tachi_ranking_parser_tests ir_ranking_modal_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(tachi_ranking_parser_tests|ir_ranking_modal_tests)$'
```

Expected: both tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/ir tests/tachi_ranking_parser_tests.cpp tests/ir_ranking_modal_tests.cpp
git commit -m "fix: hide synthesized ranking judgements"
```

---

### Task 2: Use authoritative local BP

**Files:**
- Modify: `tests/score_provenance_db_tests.cpp`
- Modify: `src/repositories/ScoreRepositoryModels.h`
- Modify: `src/repositories/ScoreRepository.h`
- Modify: `src/repositories/ScoreRepositoryInternal.h`
- Modify: `src/repositories/ScoreRepositoryQueries.cpp`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/ResultScene.cpp`

**Interfaces:**
- Consumes: stored/current BAD, POOR, and KPOOR counts.
- Produces: `ScoreBestSnapshot::badPoints` and accurate `IrLocalComparison::badPoints`.

- [ ] **Step 1: Write a failing repository regression**

Save a score state with BAD 14, POOR 8, and KPOOR 40, then require:

```cpp
const auto best = helper.LoadBestScore(meta);
assert(best && best->badPoints == 62);
```

- [ ] **Step 2: Run the repository test and verify RED**

```bash
cmake --build cmake-build-debug --target score_provenance_db_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^foundation_provenance_db$'
```

Expected: compilation fails because `ScoreBestSnapshot::badPoints` does not exist.

- [ ] **Step 3: Load BP from the selected best row**

Add `std::optional<int> badPoints` to `ScoreBestSnapshot`. Extend
`LoadBestScore` and its internal helper with a final defaulted
`selectedLongNoteMode` argument. Select `bad + poor + kpoor` beside the existing
columns, validate it fits in nonnegative `int`, and populate the optional.
Resolve the effective mode with:

```cpp
scoreLongNoteModeForClearLamp(chartMeta, selectedLongNoteMode)
```

- [ ] **Step 4: Feed corrected BP into both ranking entry points**

On result, use the current state sum. On song select, call `LoadBestScore` on
ranking open with the profile's selected LN mode and use the returned snapshot,
including `badPoints`, for `IrLocalComparison`.

Do not modify `resultState.comboBreak`, the result-screen `BREAK` card, or any
other combo-break presentation.

- [ ] **Step 5: Run the repository test and verify GREEN**

```bash
cmake --build cmake-build-debug --target score_provenance_db_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^foundation_provenance_db$'
```

Expected: the test passes with BP 62.

- [ ] **Step 6: Commit**

```bash
git add src/repositories src/scene tests/score_provenance_db_tests.cpp
git commit -m "fix: show authoritative local BP"
```

---

### Task 3: Verify the integrated fix

**Files:**
- Verify only.

**Interfaces:**
- Consumes: Tasks 1 and 2.
- Produces: build and regression evidence.

- [ ] **Step 1: Build the application and focused targets**

```bash
cmake --build cmake-build-debug --target main tachi_ranking_parser_tests ir_ranking_modal_tests score_provenance_db_tests -j 6
```

- [ ] **Step 2: Run focused and complete tests**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -R '^(tachi_ranking_parser_tests|ir_ranking_modal_tests|foundation_provenance_db|main_menu_ranking_flow_audit|ir_ranking_detail_flow_audit)$'
ctest --test-dir cmake-build-debug --output-on-failure -j 6
git diff --check
git status --short
```

Expected: the application links, focused tests pass, all CTest tests pass, and
the worktree contains only intentional committed changes.
