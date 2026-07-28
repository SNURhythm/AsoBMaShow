# Legacy Summary Records Presentation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render every available legacy summary header fact in Records without reconstructing any missing legacy detail.

**Architecture:** `ResultRecordSummary` projects optional gauge and course-progress facts from every durable record model. A header-only Records formatter owns detail, score, and secondary-score labels; the list row binds those labels without inspecting nested legacy payloads.

**Tech Stack:** C++23, existing `ResultRecordSummary` projection, header-only view formatting, bgfx Noop view integration tests, CMake/CTest.

## Global Constraints

- Legacy summaries remain Records-only and gain no detailed View Result or replay-dependent action.
- Never read or reconstruct legacy event, touch, lane-cover, course-stage, provenance, or current-chart detail.
- Preserve independently stored malformed/partial headers field by field.
- Keep modern, remote, and Auto Play presentation semantics unchanged.
- Never mutate the original `~/Downloads/profiles` directory.
- Do not deploy to TestFlight or Google Play; the already-authorized PR workflow may deploy iOS to Firebase App Distribution.

---

## File Structure

- Modify `src/ResultRecordSummary.h` to expose optional final-gauge and course-progress presentation facts.
- Modify `src/ResultRecordSummary.cpp` so every record factory projects those facts from its own durable model.
- Create `src/ResultRecordFormatting.h` as the single pure formatting boundary for Records detail, score, score-rank, and clear-lamp fallback labels.
- Modify `src/view/ResultRecordListView.h` to bind formatter output instead of hard-coded legacy labels.
- Modify `tests/result_record_summary_tests.cpp` to protect field projection.
- Modify `tests/result_record_list_view_tests.cpp` to protect visible chart, course, and empty-partial legacy rows.

### Task 1: Project Durable Presentation Facts

**Files:**
- Modify: `tests/result_record_summary_tests.cpp`
- Modify: `src/ResultRecordSummary.h`
- Modify: `src/ResultRecordSummary.cpp`

**Interfaces:**
- Produces: `ResultRecordSummary::finalGauge`, `completedCharts`, and `totalCharts`, each optional and copied only from the owning durable record.
- Consumes: existing modern result, remote score, Auto Play summary, and legacy header fields.

- [ ] **Step 1: Write the failing projection test**

Extend `testLegacySummariesExposeRecordsOnly` with literal chart and course
facts and assertions equivalent to:

```cpp
chart.finalGauge = 62.5;
chart.maxCombo = 555;
const auto chartRecord = makeLegacyChartResultRecord(chart);
expect(chartRecord.finalGauge == 62.5 && chartRecord.maxCombo == 555 &&
           !chartRecord.completedCharts && !chartRecord.totalCharts,
       "legacy chart projects independently stored presentation facts");

course.finalGauge = 48.0;
course.completedCharts = 3;
course.totalCharts = 5;
const auto courseRecord = makeLegacyCourseResultRecord(course);
expect(courseRecord.finalGauge == 48.0 &&
           courseRecord.completedCharts == 3 &&
           courseRecord.totalCharts == 5,
       "legacy course projects independently stored presentation facts");
```

- [ ] **Step 2: Run the projection test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target result_record_summary_tests -j 6
ctest --test-dir cmake-build-debug -R '^result_record_summary_tests$' --output-on-failure
```

Expected: compilation fails because the three optional presentation members do
not exist.

- [ ] **Step 3: Add the minimal projection fields**

Add these members beside `maxCombo` in `ResultRecordSummary`:

```cpp
std::optional<double> finalGauge;
std::optional<int> completedCharts;
std::optional<int> totalCharts;
```

Populate them in each factory from its durable source:

```cpp
.finalGauge = summary.finalGauge,
.completedCharts = summary.completedCharts,
.totalCharts = summary.totalCharts,
```

Use the corresponding chart score, modern course result, Auto Play replay, and
remote score fields for non-legacy factories. Chart records leave course
progress empty.

- [ ] **Step 4: Run projection tests and verify GREEN**

Run the two commands from Step 2. Expected: `result_record_summary_tests`
passes.

- [ ] **Step 5: Commit the projection**

```bash
git add src/ResultRecordSummary.h src/ResultRecordSummary.cpp tests/result_record_summary_tests.cpp
git commit -m "fix: project legacy Records presentation facts"
```

### Task 2: Format Available Facts Without Legacy Placeholders

**Files:**
- Create: `src/ResultRecordFormatting.h`
- Modify: `src/view/ResultRecordListView.h`
- Modify: `tests/result_record_list_view_tests.cpp`

**Interfaces:**
- Consumes: `ResultRecordSummary` optional projection facts from Task 1.
- Produces: `result_record_ui::detailLabel`, `scoreLabel`, `scoreRank`, and `secondaryScoreLabel`.

- [ ] **Step 1: Write the failing visible-row regressions**

Replace the existing legacy placeholder assertion with a legacy chart whose
stored facts are:

```cpp
legacySummary.finalScore = 1'432;
legacySummary.maxCombo = 555;
legacySummary.finalGauge = 62.5;
legacySummary.clearType = kClearTypeHardClearRank;
```

Assert literal visible output:

```cpp
require(rowText(*reusedRow, "recordScore")->getText() == "1432" &&
            rowText(*reusedRow, "recordRank")->getText() == "HARD CLEAR" &&
            rowText(*reusedRow, "recordDetail")->getText() ==
                "Gauge 62.5%  Combo 555",
        "legacy chart row renders every available header fact");
```

Add a legacy course row with score `2100`, gauge `48.0`, combo `321`, hard
clear, and progress `3/5`; assert detail
`Gauge 48.0%  Combo 321  Course 3/5`. Add an empty partial chart and assert
detail, score, and secondary-score labels are each `—`, with no badge or action.

- [ ] **Step 2: Run the visible-row test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target result_record_list_view_tests -j 6
ctest --test-dir cmake-build-debug -R '^result_record_list_view_tests$' --output-on-failure
```

Expected: the chart assertion fails because the row still displays
`Legacy summary` and `Unavailable`.

- [ ] **Step 3: Create the pure formatting boundary**

Create `src/ResultRecordFormatting.h` with inline functions in
`result_record_ui`:

```cpp
inline void appendDetail(std::string &detail, std::string segment) {
  if (!detail.empty()) {
    detail += "  ";
  }
  detail += std::move(segment);
}

inline std::string detailLabel(const ResultRecordSummary &summary) {
  if (summary.isLegacyChart() || summary.isLegacyCourse()) {
    std::string detail;
    if (summary.finalGauge) {
      appendDetail(detail, "Gauge " + replay_summary_ui::formatGauge(
                                          static_cast<float>(*summary.finalGauge)));
    }
    if (summary.maxCombo) {
      appendDetail(detail, "Combo " + std::to_string(*summary.maxCombo));
    }
    if (summary.completedCharts && summary.totalCharts) {
      appendDetail(detail, "Course " +
                               std::to_string(*summary.completedCharts) + "/" +
                               std::to_string(*summary.totalCharts));
    }
    return detail.empty() ? "—" : detail;
  }
  if (summary.autoPlayReplay) {
    return replay_summary_ui::detailLabel(*summary.autoPlayReplay);
  }
  std::string detail = "IR";
  if (summary.playOption && !summary.playOption->empty()) {
    detail += "  " + *summary.playOption;
  }
  return detail;
}

inline std::string scoreLabel(const ResultRecordSummary &summary) {
  if (summary.autoPlay) {
    return "AUTO";
  }
  return summary.scoreAvailable ? std::to_string(summary.score) : "—";
}

inline std::optional<std::string>
scoreRank(const ResultRecordSummary &summary) {
  if (!summary.scoreAvailable || !summary.maxScoreAvailable ||
      summary.maxScore <= 0) {
    return std::nullopt;
  }
  return score_rank::labelForScore(summary.score, summary.maxScore);
}

inline std::string secondaryScoreLabel(const ResultRecordSummary &summary) {
  if (const auto rank = scoreRank(summary)) {
    return score_rank::displayLabel(*rank);
  }
  return summary.clearRankAvailable ? clearTypeRankToLabel(summary.clearRank)
                                    : "—";
}
```

For legacy detail, append available segments in gauge, combo, course order and
join them with two spaces. Reuse `replay_summary_ui::formatGauge` for one-decimal
gauge output. Return `—` when no segment exists. Preserve the existing Auto
Play replay detail and modern/remote `IR` plus player-one option detail.

`scoreLabel` returns `AUTO`, the stored integer score, or `—`.
`scoreRank` returns a rank only when score, maximum score, and a positive
maximum score are available. `secondaryScoreLabel` returns the displayed score
rank, otherwise `clearTypeRankToLabel(summary.clearRank)` when the clear lamp is
available, otherwise `—`.

- [ ] **Step 4: Bind formatter output in the list row**

In `ResultRecordListItemView::setSummary`, replace the hard-coded detail,
score, and rank branches with the formatter. Set `currentRank` only from
`result_record_ui::scoreRank`; a clear-lamp fallback remains text-muted rather
than being sent through score-rank coloring.

- [ ] **Step 5: Run focused presentation tests and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target result_record_summary_tests result_record_list_view_tests replay_record_filters_tests -j 6
ctest --test-dir cmake-build-debug -R '^(result_record_summary_tests|result_record_list_view_tests|replay_record_filters_tests)$' --output-on-failure
```

Expected: all three tests pass.

- [ ] **Step 6: Commit the formatter and row behavior**

```bash
git add src/ResultRecordFormatting.h src/view/ResultRecordListView.h tests/result_record_list_view_tests.cpp
git commit -m "fix: show available legacy summary facts"
```

### Task 3: Verify and Publish the Review Follow-Up

**Files:**
- Modify only if a verification failure exposes an in-scope defect.

**Interfaces:**
- Consumes: the completed projection and formatting contracts.
- Produces: a clean pushed branch and updated ready PR.

- [ ] **Step 1: Run focused and full verification**

Run:

```bash
ctest --test-dir cmake-build-debug -R '^(result_record_summary_tests|result_record_list_view_tests|replay_record_filters_tests|replay_legacy_migration_tests)$' --output-on-failure
ctest --test-dir cmake-build-debug --output-on-failure
cmake --build cmake-build-debug --target main -j 6
scripts/ios_firebase_deploy.sh --build-only
```

Expected: every focused test, all 174 configured CTest tests, desktop `main`,
and the non-deploying iOS compile pass.

- [ ] **Step 2: Re-audit the legacy boundary and diff**

Run:

```bash
git diff --check origin/develop...HEAD
rg -n 'Legacy summary|Unavailable' src/view/ResultRecordListView.h tests/result_record_list_view_tests.cpp
rg -n 'SELECT|FROM|JOIN' src/repositories/ReplayRepositoryLegacySummaries.cpp
git status --short --branch
```

Expected: no legacy placeholder label remains in the Records row; repository
queries still read only summary tables; the worktree is clean after commits.

- [ ] **Step 3: Push and monitor the authorized PR workflow**

```bash
git push origin feature/file-based-replays-v2
gh pr checks 83 --watch
```

Expected: PR #83 remains open, ready, and clean; iOS Firebase App Distribution
passes, while Android remains skipped by its `develop`-only condition.
