# IR Remote Records and Partial Result Recall Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show synchronized Bokutachi scores as read-only Records entries and recall/export a result layout that displays every trustworthy supplied value while completely omitting unsupported cards and placeholders.

**Architecture:** Replace the Records modal's local-replay-only model with a tagged summary whose identity is either a local replay or an origin-scoped remote score. Build both local and remote result displays through one optional `ResultPresentationModel`; card constructors declare their required fields, so missing remote data removes the whole dependent card or row. `ResultScene` gains a read-only remote source mode while preserving the existing complete local path and course browsing behavior.

**Tech Stack:** C++23, SDL2/Yoga views, bgfx result graph rendering, SQLite, FontAwesome Solid, CMake/CTest.

## Global Constraints

- Remote Records are projections of `ir_remote_scores`; they never create canonical local `scores`, `ReplayData`, outbox drafts, attempt fingerprints, or `ScoreProvenance`.
- A remote row always shows the uploaded `IR` + check icon and is never uploadable.
- If a local replay receipt links the same provider/origin/remote score ID, suppress the single redundant remote row and keep every equivalent local replay row.
- Remote rows cannot Watch, G-Battle, Export Video, become practice/ghost sources, retry, or upload. They can View Result.
- Remote result mode exposes Back, Rankings when the normal query dependencies exist, Export Photo, and a read-only uploaded IR status. It exposes no persistence, retry, replay, practice, course, or upload controls.
- Missing data is absence, not zero. Never derive KPOOR from BP, create replay-event analytics, or claim verified provenance.
- Deterministic presentation derived solely from supplied values is allowed: EX rate/grade from score and note count; BREAK from supplied BAD + POOR; key mode from the returned BMS game; colors/labels from known lamp rank.
- A judgement card requires all five Tachi BMS totals. An early/late row requires both values for that judgement. BP remains labeled `BP`; it is never relabeled BREAK.
- Gauge history preserves null death segments. The graph draws only contiguous supplied values and carries the supplied gauge metadata/colored label when present.
- Local and course result recall must retain current values, graph cycling, controls, and export output after moving to the shared presentation model.
- A remote row deleted by later sync fails closed if selected concurrently; show a bounded message and return to the still-open Records modal.
- Follow red-green TDD and commit after each passing task. Do not deploy.

---

## File Map

- Create `src/ResultRecordSummary.h` and `.cpp`: tagged local/remote Records identity and capability model.
- Create `src/view/ResultRecordListView.h`: virtualized list item for the tagged model.
- Modify `src/ReplayRecordFilters.h`: filter/sort tagged summaries without losing local behavior.
- Modify `src/scene/MainMenuScene.h` and `.cpp`: merge/suppress summaries, bind actions, and recall remote results.
- Modify `src/repositories/ReplayRepository.h` and `ReplayRepositoryIrRemoteScores.cpp`: chart-scoped remote lookup and ID reload.
- Create `src/scene/ResultPresentationModel.h` and `.cpp`: optional card payloads and local/remote builders.
- Modify `src/skin/SkinTypes.h`, `src/skin/DefaultSkin.h`, and `src/skin/DefaultSkin.cpp`: consume the shared optional model.
- Modify `src/scene/ResultGaugeHistory.h` and `.cpp`: shared gauge-series selection/cycling.
- Modify `src/scene/ResultScene.h` and `.cpp`: add read-only remote source mode.
- Modify `src/ResultImageExporter.h` and `.cpp`: export the same partial model.
- Modify `src/CMakeLists.txt`, `src/scene/CMakeLists.txt`, root `CMakeLists.txt`, and focused test source lists.
- Create `tests/result_record_summary_tests.cpp`, `tests/result_record_list_view_tests.cpp`, and `tests/result_presentation_model_tests.cpp`.
- Modify `tests/replay_record_filters_tests.cpp`, `tests/replay_repository_tests.cpp`, and `tests/result_gauge_history_tests.cpp`.
- Create `tests/remote_result_scene_tests.cpp` and `tests/result_image_exporter_partial_tests.cpp`.

---

### Task 1: Define Tagged Records with Explicit Capabilities

**Files:**
- Create: `src/ResultRecordSummary.h`
- Create: `src/ResultRecordSummary.cpp`
- Create: `tests/result_record_summary_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
struct LocalReplayRecordId { int replayId = 0; };
struct IrRemoteRecordId {
  std::string providerId;
  std::string serverOrigin;
  std::string remoteScoreId;
};
using ResultRecordIdentity =
    std::variant<LocalReplayRecordId, IrRemoteRecordId>;

struct ResultRecordCapabilities {
  bool watch = false;
  bool gBattle = false;
  bool resultRecall = false;
  bool videoExport = false;
  bool irUpload = false;
};

struct ResultRecordSummary {
  ResultRecordIdentity identity;
  ResultRecordCapabilities capabilities;
  bool course = false;
  bool autoPlay = false;
  int score = 0;
  int maxScore = 0;
  std::optional<int> maxCombo;
  int clearRank = kClearTypeFailedRank;
  std::int64_t displayedTimeUnixMillis = 0;
  std::string displayedTime;
  std::optional<std::string> playOption;
  ir::IrRecordState irState = ir::IrRecordState::Hidden;
  std::optional<ReplaySummary> local;
  std::optional<ir::IrRemoteScore> remote;
};

[[nodiscard]] ResultRecordSummary
makeLocalResultRecord(ReplaySummary summary);
[[nodiscard]] ResultRecordSummary makeRemoteResultRecord(
    std::string_view providerId, std::string_view serverOrigin,
    ir::IrRemoteScore score);
```

- [ ] **Step 1: Write failing conversion tests**

Assert local conversion preserves all current record behavior and semantic IR state. Assert remote conversion uses `timeAchieved` or falls back to `timeAdded`, computes `maxScore = noteCount * 2`, exposes only View Result, sets `Uploaded`, and retains nullable combo/play option. Validate identity equality/hash helpers and stable display keys without using negative replay IDs or conflating remote/local rows.

- [ ] **Step 2: Register and run for RED**

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target result_record_summary_tests -j 6
```

- [ ] **Step 3: Implement the tagged model**

Keep the original `ReplaySummary` nested for local-only loaders and actions during the migration. For remote, copy only the validated stored model. Provide visitors such as `isLocal()`, `isRemote()`, `stableKey()`, and `remoteScoreId()` so UI code never branches on sentinel integers.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target result_record_summary_tests -j 6
./cmake-build-debug/result_record_summary_tests
git diff --check
git add src/ResultRecordSummary.* src/CMakeLists.txt \
  tests/result_record_summary_tests.cpp CMakeLists.txt
git commit -m "refactor: add tagged Records summaries"
```

---

### Task 2: Merge Remote Records and Suppress Linked Duplicates

**Files:**
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryIrRemoteScores.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `tests/replay_repository_tests.cpp`
- Modify: `tests/result_record_summary_tests.cpp`

**Interfaces:**

```cpp
std::vector<ir::IrRemoteScore> ListIrRemoteScoresForChart(
    std::string_view providerId, std::string_view serverOrigin,
    std::string_view chartMd5, std::string_view chartSha256);
std::optional<ir::IrRemoteScore> LoadIrRemoteScore(
    std::string_view providerId, std::string_view serverOrigin,
    std::string_view remoteScoreId);

std::vector<ResultRecordSummary> mergeResultRecords(
    std::span<const ReplaySummary> local,
    std::span<const ir::IrRemoteScore> remote,
    std::string_view providerId, std::string_view serverOrigin);
```

- [ ] **Step 1: Write repository lookup tests**

Test SHA256 match, MD5 fallback only when SHA256 is unavailable, provider/origin isolation, bounded ordering, missing ID, malformed stored row failure, and no cross-chart title-based match.

- [ ] **Step 2: Write merge/suppression tests**

Test local + unrelated remote coexist; a local receipt with the exact remote score ID suppresses one standalone remote row; multiple equivalent local attempts remain; remote IDs linked only to another origin do not suppress; a receipt without remote ID does not guess; remote rows sort by achieved/fallback-added time alongside locals; an Auto Play row remains first under the current policy.

- [ ] **Step 3: Run for RED**

```bash
cmake --build cmake-build-debug --target replay_repository_tests \
  result_record_summary_tests -j 6
./cmake-build-debug/replay_repository_tests
./cmake-build-debug/result_record_summary_tests
```

- [ ] **Step 4: Implement bounded reads and pure merge**

Query the active origin's remote rows for the selected chart after local summaries are read. Build a set of exact linked remote IDs from local receipts and omit only those standalone rows. Keep the remote mirror untouched. On reconciliation success revision, reload and merge the complete visible result set while preserving selected `stableKey` and scroll offset.

- [ ] **Step 5: Verify and commit**

```bash
cmake --build cmake-build-debug --target replay_repository_tests \
  result_record_summary_tests -j 6
./cmake-build-debug/replay_repository_tests
./cmake-build-debug/result_record_summary_tests
git diff --check
git add src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryIrRemoteScores.cpp \
  src/ResultRecordSummary.* src/scene/MainMenuScene.* \
  tests/replay_repository_tests.cpp tests/result_record_summary_tests.cpp
git commit -m "feat: merge synchronized IR scores into Records"
```

---

### Task 3: Virtualize Tagged Rows and Gate Replay-Only Actions

**Files:**
- Create: `src/view/ResultRecordListView.h`
- Modify: `src/view/ReplaySummaryListView.h`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Create: `tests/result_record_list_view_tests.cpp`
- Modify: `tests/replay_record_filters_tests.cpp`
- Modify: `src/ReplayRecordFilters.h`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write virtualized rebind tests**

Bind the same recycled item local eligible, local uploading, local uploaded, remote uploaded, then hidden. Assert all labels, FontAwesome glyph, colors, click handlers, and stable identity are replaced. Remote always renders `IR` plus `0xf00c`; tapping its badge consumes the event without upload. Row selection still invokes View Result.

Test filters and sorting on tagged summaries: clear, score, combo only when present, play option only when present, stable newest order, local course behavior, and score-rank availability. Missing remote values do not enter a matching filter as zero.

- [ ] **Step 2: Run for RED**

```bash
cmake --build cmake-build-debug --target result_record_list_view_tests \
  replay_record_filters_tests -j 6
```

- [ ] **Step 3: Implement the Records-only list view**

Keep `ReplaySummaryListView` for Chart Viewer/practice ghost use. Use `ResultRecordListView` only in the Main Menu Records modal. Route upload badge actions only for local records whose capability allows it. Base footer visibility/enabled state on `ResultRecordCapabilities`, not scattered `isRemote` checks:

- local replay: current capabilities;
- local course: current stage/course result browsing behavior;
- remote: View Result only;
- no selection: no action.

Preserve the existing icon-only modal close button and outside-touch blocking behavior.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target result_record_list_view_tests \
  replay_record_filters_tests -j 6
./cmake-build-debug/result_record_list_view_tests
./cmake-build-debug/replay_record_filters_tests
git diff --check
git add src/view/ResultRecordListView.h src/view/ReplaySummaryListView.h \
  src/ReplayRecordFilters.h src/scene/MainMenuScene.* \
  tests/result_record_list_view_tests.cpp tests/replay_record_filters_tests.cpp \
  CMakeLists.txt
git commit -m "feat: gate Records actions by record capability"
```

---

### Task 4: Build One Optional Result Presentation Model

**Files:**
- Create: `src/scene/ResultPresentationModel.h`
- Create: `src/scene/ResultPresentationModel.cpp`
- Create: `tests/result_presentation_model_tests.cpp`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
struct ResultGaugeSeries {
  std::vector<std::optional<float>> points;
  std::optional<std::string> label;
  std::optional<int> clearRank;
};

struct ResultJudgementRow {
  std::string label;
  Color color;
  int total = 0;
  std::optional<int> early;
  std::optional<int> late;
};

struct ResultComparisonValue {
  std::string label;
  std::string value;
  std::string detail;
  Color accent;
};

struct ResultComparisonCard {
  std::string title;
  std::optional<ResultComparisonValue> target;
  ResultComparisonValue current;
  std::optional<std::string> delta;
};

struct ResultInfoTile {
  std::string label;
  std::string value;
  std::optional<std::string> detail;
  Color accent;
};

struct ResultLocalPresentationOptions {
  std::string playModeLabel;
  std::string laneOrderLabel;
  std::string difficultyLabel;
  std::optional<std::string> headerDifficultyLabelOverride;
  std::optional<std::string> currentClearLabelOverride;
  std::optional<int> currentClearRankOverride;
  std::optional<ResultPreviousBestData> previousBest;
  std::optional<ResultPacemakerData> pacemaker;
  std::optional<practice::ResultModel> timingAnalytics;
};

struct ResultPresentationModel {
  std::string title;
  std::optional<std::string> artist;
  std::optional<std::string> difficulty;
  std::optional<std::string> playtype;
  std::optional<std::int64_t> achievedAtUnixMillis;
  std::optional<std::string> service;
  std::optional<std::string> client;
  std::optional<std::string> inputDevice;
  std::optional<std::string> random;
  std::optional<std::string> gaugeType;
  std::optional<int> score;
  std::optional<int> maxScore;
  std::optional<int> lampRank;
  std::optional<float> finalGauge;
  std::optional<int> maxCombo;
  std::optional<int> comboBreak;
  std::optional<int> badPoints;
  std::optional<ResultComparisonCard> scoreComparison;
  std::optional<ResultComparisonCard> lampComparison;
  std::optional<ResultComparisonCard> comboComparison;
  std::vector<ResultInfoTile> infoTiles;
  std::vector<ResultJudgementRow> judgements;
  std::optional<int> fast;
  std::optional<int> slow;
  std::vector<ResultGaugeSeries> gaugeSeries;
  std::optional<practice::ResultModel> timingAnalytics;
  bool readOnlyIrUploaded = false;
};

[[nodiscard]] ResultPresentationModel makeLocalResultPresentation(
    const bms_parser::ChartMeta &, const RhythmState &,
    ResultLocalPresentationOptions);
[[nodiscard]] ResultPresentationModel makeRemoteResultPresentation(
    const ir::IrRemoteScore &);
```

- [ ] **Step 1: Write local parity tests**

Build representative normal, GAS, failed, full-combo, replay recall, and saved course stage/final models. Assert every value currently rendered by `DefaultSkin` remains present, local KPOOR remains present, local BREAK remains BAD + POOR without KPOOR, GAS series starts with the adopted gauge and retains cycling order, and current comparisons/pacemaker fields remain unchanged. These tests are the guardrail before changing skin code.

- [ ] **Step 2: Write remote omission tests**

From a fully populated remote score, assert title/artist/playtype, score/max/rate/grade dependencies, lamp, complete five-judgement rows, early/late pairs, combo, derived BREAK only when both BAD and POOR exist, BP, gauge, metadata, and gauge history. Then remove one dependency at a time:

- missing note count removes grade/rate/max-dependent score presentation;
- any missing judgement total removes the entire judgement card;
- one missing early/late side removes only that timing row;
- missing combo/BP/final gauge/history removes only its dependent tile/card;
- missing metadata removes its tile;
- KPOOR is absent under every remote input;
- analytics is absent under every remote input.

Assert missing and explicit zero are distinguishable.

- [ ] **Step 3: Run for RED**

```bash
cmake --build cmake-build-debug --target result_presentation_model_tests -j 6
```

- [ ] **Step 4: Implement builders and explicit card predicates**

Centralize the canonical judgement extraction/mapping here; neither ranking details nor remote result UI should re-parse Tachi field names. Provide named predicates such as `hasGradeCard`, `hasJudgementCard`, `hasComboBreakCard`, `hasGaugeCard`, and `timingRows`. The remote builder computes only deterministic values listed in Global Constraints.

- [ ] **Step 5: Verify and commit**

```bash
cmake --build cmake-build-debug --target result_presentation_model_tests -j 6
./cmake-build-debug/result_presentation_model_tests
git diff --check
git add src/scene/ResultPresentationModel.* src/scene/CMakeLists.txt \
  tests/result_presentation_model_tests.cpp CMakeLists.txt
git commit -m "refactor: model optional result presentation cards"
```

---

### Task 5: Make the Default Result Skin Omit Missing Cards

**Files:**
- Modify: `src/skin/SkinTypes.h`
- Modify: `src/skin/DefaultSkin.h`
- Modify: `src/skin/DefaultSkin.cpp`
- Modify: `tests/result_presentation_model_tests.cpp`
- Create: `scripts/check_partial_result_layout.py`

**Interfaces:**

Append without changing existing aggregate initialization order:

```cpp
struct ResultSkinData {
  // existing fields remain in place
  const ResultPresentationModel *presentation = nullptr;
};
```

- [ ] **Step 1: Add layout-contract tests**

Build a complete local model and a series of partial remote models through `DefaultSkin`. Inspect named view descendants and assert absent cards/tiles do not exist and consume no width/height. Assert present summary cards flex evenly, a one-card row expands cleanly, metadata tiles wrap within mobile metrics, and local named views remain available to current tests.

Add `scripts/check_partial_result_layout.py` to prohibit direct remote fallback expressions such as `value_or(0)`, dummy `RhythmState`, dummy `ScoreProvenance`, or placeholder `--` inside the presentation override path.

- [ ] **Step 2: Run for RED**

```bash
cmake --build cmake-build-debug --target result_presentation_model_tests -j 6
./cmake-build-debug/result_presentation_model_tests
python3 scripts/check_partial_result_layout.py
```

- [ ] **Step 3: Refactor the skin to consume the model**

Make `ResultSkinData::presentation` authoritative when provided. Move current local layout values behind `makeLocalResultPresentation`, then use the same card factories for local and remote. Add a card only when its predicate passes; do not add empty dividers or fixed-height placeholder rows. Keep timing analytics and raw-event lower panels only when the model supplies them.

Preserve styling, semantic colors, local comparison content, and responsive result metrics. Give metadata labels explicit names for testability.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target result_presentation_model_tests -j 6
./cmake-build-debug/result_presentation_model_tests
python3 scripts/check_partial_result_layout.py
git diff --check
git add src/skin/SkinTypes.h src/skin/DefaultSkin.* \
  tests/result_presentation_model_tests.cpp scripts/check_partial_result_layout.py
git commit -m "refactor: omit unavailable result cards"
```

---

### Task 6: Share Nullable Gauge Rendering and Local GAS Cycling

**Files:**
- Modify: `src/scene/ResultGaugeHistory.h`
- Modify: `src/scene/ResultGaugeHistory.cpp`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/ResultImageExporter.cpp`
- Modify: `tests/result_gauge_history_tests.cpp`
- Modify: `tests/result_presentation_model_tests.cpp`

**Interfaces:**

```cpp
[[nodiscard]] std::vector<ResultGaugeSeries>
seriesFor(const GameplayScoreState &state);
[[nodiscard]] std::size_t nextSeriesIndex(
    std::span<const ResultGaugeSeries> series, std::size_t current);
```

- [ ] **Step 1: Add graph and cycling regressions**

Assert local GAS still begins at the final adopted/cleared gauge, cycles through every available gauge on tap, and uses the correct colored gauge label. Assert a remote series containing values, null run, then values emits two line strips and no connecting segment across nulls. Empty/all-null history creates no graph card. Single-point history renders one marker. Scene and image exporter choose the same series and label.

- [ ] **Step 2: Run for RED**

```bash
cmake --build cmake-build-debug --target result_gauge_history_tests \
  result_presentation_model_tests -j 6
./cmake-build-debug/result_gauge_history_tests
./cmake-build-debug/result_presentation_model_tests
```

- [ ] **Step 3: Implement one graph primitive**

Move graph geometry/color selection to a helper that accepts `ResultGaugeSeries`. Draw a segment only when both adjacent points are present; markers only for present points. Scene and exporter call this helper. Local histories convert `float` to present optional values; remote histories preserve nulls. The label color comes from supplied/known gauge or lamp semantics, never an invented gauge type.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target result_gauge_history_tests \
  result_presentation_model_tests -j 6
./cmake-build-debug/result_gauge_history_tests
./cmake-build-debug/result_presentation_model_tests
git diff --check
git add src/scene/ResultGaugeHistory.* src/scene/ResultScene.cpp \
  src/ResultImageExporter.cpp tests/result_gauge_history_tests.cpp \
  tests/result_presentation_model_tests.cpp
git commit -m "refactor: share nullable result gauge rendering"
```

---

### Task 7: Add Read-Only Remote ResultScene Recall

**Files:**
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Create: `tests/remote_result_scene_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
struct ResultRemoteOptions {
  ir::IrRemoteScore score;
  ir::IrChartQuery rankingQuery;
};

ResultScene(ApplicationContext &context, ResultRemoteOptions remote);
```

Internally use a source variant so remote mode owns no fabricated local state:

```cpp
std::variant<LocalResultSource, RemoteResultSource> source;
```

- [ ] **Step 1: Write action/state tests**

Assert the remote constructor builds only from `IrRemoteScore`; no `ReplayData`, `RhythmState`, `ScoreProvenance`, or raw previous-scene pointer is accepted. Remote init shows Back, Rankings when query hashes/game are valid, Export Photo, and uploaded IR check. It omits persistence, retry, replay, retry-same, practice, course, and IR upload/retry controls. Ranking opens for the exact stored game/hash. Back pops the retained scene stack and restores the Records modal selection.

Assert local constructor and course browsing retain existing action matrices and that local IR upload controls still appear for eligible recalled local scores.

- [ ] **Step 2: Run for RED**

```bash
cmake --build cmake-build-debug --target remote_result_scene_tests -j 6
./cmake-build-debug/remote_result_scene_tests
```

- [ ] **Step 3: Implement source-dispatched behavior**

Convert existing local members into `LocalResultSource` and guard local-only methods through a typed accessor; do not default-construct fake local objects. `makeResultSkinData` always points at the source's `ResultPresentationModel`; legacy state/meta pointers remain populated only for local compatibility until the skin refactor is complete.

In `MainMenuScene`, reload the selected remote score by exact origin-scoped ID immediately before a retaining scene transition. If absent or invalid, call the existing bounded recall-failure feedback, keep Records open, and reload the merged list. Construct the ranking query only from validated stored game/hashes. Remote Back uses the same retained-scene mechanism as existing recalled results, avoiding a dangling raw scene pointer.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target remote_result_scene_tests -j 6
./cmake-build-debug/remote_result_scene_tests
python3 scripts/check_partial_result_layout.py
git diff --check
git add src/scene/ResultScene.* src/scene/MainMenuScene.* \
  tests/remote_result_scene_tests.cpp CMakeLists.txt
git commit -m "feat: recall read-only IR result scenes"
```

---

### Task 8: Export the Exact Partial Result Layout

**Files:**
- Modify: `src/ResultImageExporter.h`
- Modify: `src/ResultImageExporter.cpp`
- Modify: `src/scene/ResultScene.cpp`
- Create: `tests/result_image_exporter_partial_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
static ResultImageExportResult Export(
    ApplicationContext &context, const ResultPresentationModel &presentation);
```

- [ ] **Step 1: Write partial-export tests**

Export a complete remote model and a sparse score/lamp-only model into a temporary output target. Assert the exporter uses the same named card set and nullable gauge segments as the scene, chooses a safe title-based filename, and does not render missing judgement/combo/gauge/metadata/analytics placeholders. Keep existing local and course export snapshots/contracts green.

- [ ] **Step 2: Run for RED**

```bash
cmake --build cmake-build-debug --target result_image_exporter_partial_tests -j 6
./cmake-build-debug/result_image_exporter_partial_tests
```

- [ ] **Step 3: Implement the presentation overload**

Share the same `ResultSkinData` + `DefaultSkin` layout and gauge primitive as the interactive scene. Remote `ResultScene::exportPhoto` passes its immutable model. Do not construct a temporary `RhythmState` or chart metadata to satisfy the exporter.

- [ ] **Step 4: Run focused and full verification**

```bash
cmake --build cmake-build-debug --target result_record_summary_tests \
  result_record_list_view_tests replay_record_filters_tests \
  result_presentation_model_tests result_gauge_history_tests \
  remote_result_scene_tests result_image_exporter_partial_tests \
  replay_repository_tests -j 6
./cmake-build-debug/result_record_summary_tests
./cmake-build-debug/result_record_list_view_tests
./cmake-build-debug/replay_record_filters_tests
./cmake-build-debug/result_presentation_model_tests
./cmake-build-debug/result_gauge_history_tests
./cmake-build-debug/remote_result_scene_tests
./cmake-build-debug/result_image_exporter_partial_tests
./cmake-build-debug/replay_repository_tests
python3 scripts/check_partial_result_layout.py
ctest --test-dir cmake-build-debug --output-on-failure
cmake --build cmake-build-debug --target main -j 6
git diff --check
```

- [ ] **Step 5: Commit**

```bash
git add src/ResultImageExporter.* src/scene/ResultScene.cpp \
  tests/result_image_exporter_partial_tests.cpp CMakeLists.txt
git commit -m "feat: export partial imported IR results"
```

---

## Plan 3 Completion Gate

- Records shows every unsuppressed synchronized remote score with an uploaded checkmark.
- Linked local replays retain richer rows and all equivalent local attempts remain visible.
- Replay-only actions cannot be invoked for a remote row through button, keyboard, or stale virtualized callback.
- Remote recall shows only supplied/deterministic cards, never KPOOR or event-derived analytics.
- Scene and photo export use the same responsive card set and nullable gauge renderer.
- Existing local result recall, GAS graph cycling, course stage-to-total browsing, IR controls, and exports remain behaviorally unchanged.
