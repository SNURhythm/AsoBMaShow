# Records Result Recall Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Records modal photo action with a safe `View Result` flow for saved chart and course replays, including historical Bokutachi submission controls only when durable attempt metadata validates exactly.

**Architecture:** Extend `ReplayRepository` with a snapshot-consistent chart-result record that carries replay data plus nullable attempt integrity metadata and the original database timestamp. A focused `ResultRecallBuilder` reconstructs chart states and all-or-nothing course browse sessions; `MainMenuScene` launches the existing `ResultScene`, while a dedicated course browse flag makes `Next` navigate reconstructed results without gameplay or auto-advance.

**Tech Stack:** C++23, SQLite, SDL2, existing BMS parser/play-option helpers, Yoga-based scene UI, CMake/CTest, assert-style C++ test executables.

## Global Constraints

- Replace the Records modal's `Export Photo` action with `View Result`.
- Support saved single-chart replays and saved course replays.
- Keep synthetic Auto Play entries disabled because they are not saved player results.
- Preserve `Watch`, `G-BATTLE`, and `Export Video` behavior.
- Bokutachi submission remains limited to eligible single-chart LR2 results; do not add course-score submission.
- Legacy records without durable attempt identity remain viewable but cannot expose a new submission action.
- A fingerprint mismatch never creates or mutates an IR outbox row.
- Opening a recalled result is read-only; only the existing `Submit` or `Retry` action may mutate the outbox.
- Existing outbox rows remain authoritative and API credentials must never enter replay, recalled-result, or outbox payload models.
- Show an amber `IR ↑` badge for canonical, Bokutachi-eligible single-chart records until their Bokutachi outbox state reaches `Succeeded`; keep it visible for absent, queued, uploading, polling, blocked, and failed states.
- Derive Records-row markers from the summary snapshot without chart parsing, replay reconstruction, or one database call per row.
- Course reconstruction is all-or-nothing before the first stage is shown.
- Saved course browsing advances manually from stage 1 through the last stage and then the aggregate result; it never launches gameplay or auto-advances.

---

### Task 1: Snapshot-consistent replay result records

**Files:**
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryRecords.cpp`
- Modify: `tests/replay_repository_tests.cpp`

**Interfaces:**
- Consumes: existing `ReplayData`, replay chart-identity matching, `loadReplayFromConnection`, and replay read transactions.
- Produces: `ReplayResultRecord` and `ReplayRepository::LoadReplayResult(int, const bms_parser::ChartMeta &)`, used by Task 3 and Task 6.

- [ ] **Step 1: Write the failing repository tests**

Add a test next to `testChartAndCourseRoundTripAndPathIsolation` that saves a replay, writes canonical attempt metadata and a fixed UTC timestamp, and verifies the result read returns all fields atomically:

```cpp
void testReplayResultRecordMetadata(const std::filesystem::path &root) {
  const auto path = root / "result-recall" / "replay.db";
  ReplayRepository repository(path);
  assert(repository.EnsureSchema());

  ReplayData replay = sampleReplay(root, "result-recall-chart");
  const auto replayId = repository.SaveReplay(replay);
  assert(replayId.has_value());

  auto db = openDatabase(path);
  execOrAbort(
      db.get(),
      "UPDATE replays SET "
      "attempt_id='123e4567-e89b-42d3-a456-426614174000',"
      "attempt_fingerprint='0123456789abcdef0123456789abcdef"
      "0123456789abcdef0123456789abcdef',"
      "created_at='2026-07-19 03:04:05' WHERE id=" +
          std::to_string(*replayId));
  db.reset();

  const auto recalled =
      repository.LoadReplayResult(*replayId, replay.chartMeta);
  assert(recalled.has_value());
  assert(recalled->replay.id == *replayId);
  assert(recalled->attemptId ==
         "123e4567-e89b-42d3-a456-426614174000");
  assert(recalled->attemptFingerprint ==
         "0123456789abcdef0123456789abcdef"
         "0123456789abcdef0123456789abcdef");
  assert(recalled->playedAtUnixMillis == 1784430245000LL);

  execOrAbort(openDatabase(path).get(),
              "UPDATE replays SET attempt_id=NULL,"
              "attempt_fingerprint=NULL WHERE id=" +
                  std::to_string(*replayId));
  const auto legacy = repository.LoadReplayResult(*replayId, replay.chartMeta);
  assert(legacy.has_value());
  assert(!legacy->attemptId.has_value());
  assert(!legacy->attemptFingerprint.has_value());
}
```

Register the function in the test executable's `main` alongside the other replay round-trip tests.

- [ ] **Step 2: Run the focused test and verify it fails**

Run:

```bash
cmake --build cmake-build-debug --target replay_repository_tests -j 6
```

Expected: compilation fails because `ReplayResultRecord` and `LoadReplayResult` do not exist.

- [ ] **Step 3: Add the public result record and repository API**

Add this model after `ReplaySummary` in `ReplayRepository.h`:

```cpp
struct ReplayResultRecord {
  ReplayData replay;
  std::optional<std::string> attemptId;
  std::optional<std::string> attemptFingerprint;
  std::int64_t playedAtUnixMillis = 0;
};
```

Add `<cstdint>` to the header and add this method beside `LoadReplay`:

```cpp
std::optional<ReplayResultRecord>
LoadReplayResult(int replayId, const bms_parser::ChartMeta &chartMeta);
```

- [ ] **Step 4: Make the internal replay loader return replay and integrity metadata from one row**

Change `loadReplayFromConnection` to return `std::optional<ReplayResultRecord>`. Extend its replay-row query with the following three expressions after `provenance_json`:

```sql
, attempt_id, attempt_fingerprint,
CASE
  WHEN typeof(created_at) = 'text'
  THEN COALESCE(CAST(strftime('%s', created_at) AS INTEGER) * 1000, 0)
  ELSE 0
END
```

Decode nullable text without manufacturing empty strings:

```cpp
ReplayResultRecord result;
result.replay = std::move(loaded);
if (sqlite3_column_type(stmt.get(), 25) == SQLITE_TEXT) {
  result.attemptId = readText(stmt.get(), 25);
}
if (sqlite3_column_type(stmt.get(), 26) == SQLITE_TEXT) {
  result.attemptFingerprint = readText(stmt.get(), 26);
}
if (sqlite3_column_type(stmt.get(), 27) == SQLITE_INTEGER) {
  result.playedAtUnixMillis = sqlite3_column_int64(stmt.get(), 27);
}
```

Load events, touches, and lane-cover events into `result.replay`. Keep `LoadReplay` source-compatible by returning `std::move(result->replay)`, and make course-stage loading do the same. Implement `LoadReplayResult` with the same `ReadGuard`, session mutex, `BEGIN TRANSACTION`, and commit behavior as `LoadReplay`; return the full record only after the snapshot commits.

- [ ] **Step 5: Run the repository test**

Run:

```bash
cmake --build cmake-build-debug --target replay_repository_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^replay_repository_tests$'
```

Expected: build succeeds and `replay_repository_tests` passes.

- [ ] **Step 6: Commit the repository boundary**

```bash
git add src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryRecords.cpp \
  tests/replay_repository_tests.cpp
git commit -m "feat: load durable replay result metadata"
```

---

### Task 2: Records-row Bokutachi upload marker

**Files:**
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryInternal.h`
- Modify: `src/repositories/ReplayRepositoryRecords.cpp`
- Modify: `src/ir/tachi/TachiBatchManual.h`
- Modify: `src/ir/tachi/TachiBatchManual.cpp`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/view/ReplaySummaryListView.h`
- Modify: `tests/replay_repository_tests.cpp`
- Modify: `tests/tachi_batch_manual_tests.cpp`
- Create: `scripts/check_records_ir_marker.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: canonical replay attempt metadata, already-decoded `ScoreProvenance`, an indexed provider/attempt outbox lookup, and Bokutachi's existing eligibility validator.
- Produces: `ReplaySummary::irUploadPending` and an amber `IR ↑` virtualized-row badge; no chart parsing, replay reconstruction, or one-call-per-row lookup is allowed.

- [ ] **Step 1: Write failing Bokutachi marker-policy tests**

Add this test beside the existing Tachi Batch Manual eligibility tests:

```cpp
void testReplayUploadMarkerPolicy() {
  const ir::IrSubmission submission = validSubmission();
  bms_parser::ChartMeta meta;
  meta.KeyMode = submission.keyMode;
  meta.MD5 = submission.chartMd5;
  meta.SHA256 = submission.chartSha256;
  meta.TotalNotes = submission.maxScore / 2;

  const auto show = [&](std::optional<ir::IrOutboxState> state,
                        const ScoreProvenance &provenance,
                        std::string_view attemptId =
                            "123e4567-e89b-42d3-a456-426614174000",
                        bool hasFingerprint = true) {
    return ir::tachi::shouldShowReplayUploadMarker(
        attemptId, hasFingerprint, meta, provenance, state);
  };

  expect(show(std::nullopt, submission.provenance),
         "eligible result without an outbox row shows the marker");
  for (const auto state : {ir::IrOutboxState::Pending,
                           ir::IrOutboxState::Uploading,
                           ir::IrOutboxState::AwaitingRemoteResult,
                           ir::IrOutboxState::BlockedConfiguration,
                           ir::IrOutboxState::FailedPermanent}) {
    expect(show(state, submission.provenance),
           "unfinished outbox result shows the marker");
  }
  expect(!show(ir::IrOutboxState::Succeeded, submission.provenance),
         "successful outbox result hides the marker");
  expect(!show(std::nullopt, submission.provenance, "", true),
         "missing attempt identity hides the marker");
  expect(!show(std::nullopt, submission.provenance,
               "123e4567-e89b-42d3-a456-426614174000", false),
         "missing canonical fingerprint hides the marker");

  ScoreProvenance modified = submission.provenance;
  modified.assistOption = assist_options::kDrag;
  modified.eligibility = ScoreEligibility::Modified;
  expect(!show(std::nullopt, modified),
         "modified result hides the marker");
  ScoreProvenance beatoraja = submission.provenance;
  beatoraja.ruleset = RulesetDescriptor::For(GameplayRuleset::Beatoraja);
  expect(!show(std::nullopt, beatoraja),
         "Beatoraja result hides the marker");
}
```

Call `testReplayUploadMarkerPolicy()` from the test executable's `main`.

- [ ] **Step 2: Write failing summary snapshot tests**

Extend `testReplayResultRecordMetadata` in `replay_repository_tests.cpp`. After setting the replay's attempt columns, verify the requested provider state is returned in the same summary read:

```cpp
auto summaries = repository.ListReplays(replay.chartMeta, 0, "tachi");
assert(summaries.size() == 1);
assert(summaries.front().attemptId ==
       "123e4567-e89b-42d3-a456-426614174000");
assert(summaries.front().hasCanonicalAttemptFingerprint);
assert(summaries.front().provenance != nullptr);
assert(!summaries.front().requestedIrOutboxState.has_value());

db = openDatabase(path);
execOrAbort(
    db.get(),
    "INSERT INTO ir_outbox("
    "provider_id,attempt_id,chart_md5,chart_sha256,payload_json,"
    "ruleset_id,ruleset_revision,validation_fingerprint,state,"
    "local_result_ready,created_at_ms,updated_at_ms,completed_at_ms) VALUES("
    "'tachi','123e4567-e89b-42d3-a456-426614174000',"
    "'0123456789abcdef0123456789abcdef',"
    "'0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef','{}','lr2',3,"
    "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',5,1,1,1,1)");
db.reset();

summaries = repository.ListReplays(replay.chartMeta, 0, "tachi");
assert(summaries.size() == 1);
assert(summaries.front().requestedIrOutboxState ==
       ir::IrOutboxState::Succeeded);
```

- [ ] **Step 3: Add the failing row-flow audit and run all three checks**

Create `scripts/check_records_ir_marker.py`:

```python
from pathlib import Path
import sys

root = Path(sys.argv[1])
repository = (root / "src/repositories/ReplayRepositoryRecords.cpp").read_text()
menu = (root / "src/scene/MainMenuScene.cpp").read_text()
view = (root / "src/view/ReplaySummaryListView.h").read_text()
required = {
    "repository": ["requestedIrOutboxState", "irProviderId", "ir_outbox"],
    "menu": ["shouldShowReplayUploadMarker", "irUploadPending"],
    "view": ["IR ↑", "irUploadPending", "ui_theme::amber"],
}
texts = {"repository": repository, "menu": menu, "view": view}
missing = []
for group, tokens in required.items():
    missing.extend(group + ":" + token
                   for token in tokens if token not in texts[group])
if missing:
    raise SystemExit("missing Records IR marker contracts: " +
                     ", ".join(missing))
```

Register `records_ir_marker_audit` beside the existing Python flow audits.

Run:

```bash
cmake --build cmake-build-debug --target tachi_batch_manual_tests \
  replay_repository_tests -j 6
python3 scripts/check_records_ir_marker.py .
```

Expected: C++ compilation fails because the marker API and summary fields do not exist, and the audit reports the missing marker contracts.

- [ ] **Step 4: Extend replay summaries with generic requested-provider state**

Add these fields to `ReplaySummary`:

```cpp
std::shared_ptr<const ScoreProvenance> provenance;
std::optional<std::string> attemptId;
bool hasCanonicalAttemptFingerprint = false;
std::optional<ir::IrOutboxState> requestedIrOutboxState;
bool irUploadPending = false;
```

Extend the public and internal `ListReplays` signatures without changing existing callers:

```cpp
std::vector<ReplaySummary> ListReplays(
    const bms_parser::ChartMeta &chartMeta, int limit = 100,
    std::string_view irProviderId = {});
```

The detail statement must select `attempt_id`, `attempt_fingerprint`, and one indexed scalar state lookup:

```sql
(SELECT o.state FROM ir_outbox o
 WHERE o.provider_id = ? AND o.attempt_id = r.attempt_id
 LIMIT 1)
```

When no provider ID is requested, bind an impossible empty provider ID and leave `requestedIrOutboxState` empty. Move the provenance already decoded by `readStoredProvenance` into a `shared_ptr`, retain an attempt ID only when it is a canonical lower UUID v4, set `hasCanonicalAttemptFingerprint` only for a 64-character lowercase hex value, and decode the outbox state only when `ir::isKnownIrOutboxState` succeeds. Keep all of this inside the existing summary snapshot; do not add a loop that calls `LoadIrOutbox`.

- [ ] **Step 5: Add the pure Bokutachi marker decision**

Declare in `TachiBatchManual.h`:

```cpp
[[nodiscard]] bool shouldShowReplayUploadMarker(
    std::string_view attemptId, bool hasCanonicalAttemptFingerprint,
    const bms_parser::ChartMeta &meta, const ScoreProvenance &provenance,
    std::optional<IrOutboxState> outboxState) noexcept;
```

Implement it by rejecting a missing/noncanonical attempt, missing fingerprint, and `Succeeded`, then constructing an eligibility probe from the chart metadata and stored provenance:

```cpp
bool shouldShowReplayUploadMarker(
    std::string_view attemptId, bool hasCanonicalAttemptFingerprint,
    const bms_parser::ChartMeta &meta, const ScoreProvenance &provenance,
    std::optional<IrOutboxState> outboxState) noexcept {
  if (!uuid::isCanonicalLowerV4(attemptId) ||
      !hasCanonicalAttemptFingerprint ||
      outboxState == IrOutboxState::Succeeded || meta.TotalNotes <= 0 ||
      meta.TotalNotes > std::numeric_limits<int>::max() / 2) {
    return false;
  }
  IrSubmission probe;
  probe.attemptId = std::string(attemptId);
  probe.keyMode = meta.KeyMode;
  probe.chartMd5 = normalizedHash(meta.MD5);
  probe.chartSha256 = normalizedHash(meta.SHA256);
  probe.maxScore = meta.TotalNotes * 2;
  probe.provenance = provenance;
  return validateBokutachiEligibility(probe).reason ==
         SubmissionEligibilityReason::Eligible;
}
```

Use a local trim-and-lowercase `normalizedHash` helper so chart hashes compare canonically with provenance. This function performs no parsing, payload construction, database access, or credential access.

- [ ] **Step 6: Annotate MainMenu summaries and render the badge**

Request Bokutachi state when loading normal chart records:

```cpp
replaySummaries = context.replayRepository.ListReplays(
    record.meta, 0, ir::kTachiProviderId);
for (ReplaySummary &summary : replaySummaries) {
  if (!summary.autoPlay && !summary.courseReplay && summary.chartMeta &&
      summary.provenance && summary.attemptId) {
    summary.irUploadPending = ir::tachi::shouldShowReplayUploadMarker(
        *summary.attemptId, summary.hasCanonicalAttemptFingerprint,
        *summary.chartMeta, *summary.provenance,
        summary.requestedIrOutboxState);
  }
}
```

In `ReplaySummaryListItemView`, create an `irBadge` `TextView` after `textColumn` and before `scoreColumn`. Give it text `IR ↑`, height 28, width 54, centered alignment, corner radius 6, amber background, and `ui_theme::sdl(ui_theme::textOn(ui_theme::amber()))` text. In `setSummary`, use both visibility and width so recycled rows cannot leak the previous item's marker:

```cpp
const bool showIrBadge = summary.irUploadPending;
irBadge->setVisible(showIrBadge);
irBadge->setWidth(showIrBadge ? 54.0F : 0.0F);
```

Do not alter selection colors or the existing score/rank column.

- [ ] **Step 7: Run the marker tests and audit**

Run:

```bash
cmake --build cmake-build-debug --target tachi_batch_manual_tests \
  replay_repository_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(tachi_batch_manual_tests|replay_repository_tests|records_ir_marker_audit)$'
```

Expected: policy tests cover absent and every non-success outbox state, repository tests prove provider state is snapshot-loaded, and the row audit passes.

- [ ] **Step 8: Commit the list marker**

```bash
git add src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryInternal.h \
  src/repositories/ReplayRepositoryRecords.cpp \
  src/ir/tachi/TachiBatchManual.h src/ir/tachi/TachiBatchManual.cpp \
  src/scene/MainMenuScene.cpp src/view/ReplaySummaryListView.h \
  tests/replay_repository_tests.cpp tests/tachi_batch_manual_tests.cpp \
  scripts/check_records_ir_marker.py CMakeLists.txt
git commit -m "feat: mark pending Bokutachi record uploads"
```

---

### Task 3: Single-chart result reconstruction and historical IR validation

**Files:**
- Create: `src/ResultRecallBuilder.h`
- Create: `src/ResultRecallBuilder.cpp`
- Create: `tests/result_recall_builder_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ReplayResultRecord` from Task 1, `play_options::prepareReplayChart`, `replay_result::BuildResultState`, `makeChartResultAttempt`, and `ir::makeIrSubmission`.
- Produces: `result_recall::ChartBuildOutcome`, containing an always-viewable reconstructed result and an optional validated `HistoricalIrContext`.

- [ ] **Step 1: Declare the focused builder API in the failing test**

Create `tests/result_recall_builder_tests.cpp` with a deterministic one-note replay fixture and an injected chart loader:

```cpp
#include "../src/ResultRecallBuilder.h"
#include "../src/ResultPersistenceModel.h"
#include "../src/ReplayResultStateBuilder.h"

#include <atomic>
#include <cassert>
#include <memory>

namespace {

constexpr const char *kAttemptId =
    "123e4567-e89b-42d3-a456-426614174000";

ReplayResultRecord validRecord() {
  ReplayResultRecord record;
  record.replay.id = 41;
  record.replay.chartMeta.BmsPath = "recall.bms";
  record.replay.chartMeta.MD5 = "0123456789abcdef0123456789abcdef";
  record.replay.chartMeta.SHA256 =
      "0123456789abcdef0123456789abcdef"
      "0123456789abcdef0123456789abcdef";
  record.replay.chartMeta.KeyMode = 7;
  record.replay.chartMeta.TotalNotes = 1;
  record.replay.initialGaugeType = GaugeType::Normal;
  record.replay.finalScore = 2;
  record.replay.maxCombo = 1;
  record.replay.finalGauge = 100.0F;
  record.replay.clearType = kClearTypeFullComboRank;
  record.replay.provenance = ScoreProvenance::Legacy();
  record.replay.provenance.schemaVersion = ScoreProvenance::kSchemaVersion;
  record.replay.provenance.ruleset =
      RulesetDescriptor::For(GameplayRuleset::LR2);
  record.replay.provenance.gaugeType = GaugeType::Normal;
  record.replay.provenance.eligibility = ScoreEligibility::Verified;
  record.replay.events.push_back(
      {.action = ReplayEventAction::Press,
       .lane = 0,
       .noteTimeMicros = 1000,
       .songTimeMicros = 1000,
       .judgeTimeMicros = 1000,
       .judgement = PGreat,
       .diffMicros = 0,
       .gauge = 100.0F,
       .gaugeType = GaugeType::Normal,
       .combo = 1,
       .score = 2});
  record.attemptId = kAttemptId;
  record.playedAtUnixMillis = 1784420645000LL;

  bms_parser::Chart chart;
  chart.Meta = record.replay.chartMeta;
  RhythmState state = replay_result::BuildResultState(chart, record.replay);
  std::string diagnostic;
  auto attempt = result_persistence::makeChartResultAttempt(
      kAttemptId, chart.Meta, state, record.replay.provenance,
      record.replay.chartMeta.LnMode, record.replay, diagnostic);
  assert(attempt.has_value());
  record.attemptFingerprint = attempt->payloadFingerprint;
  return record;
}

result_recall::ReplayChartLoader chartLoader() {
  return [](const ReplayData &replay, std::atomic_bool &) {
    auto chart = std::make_unique<bms_parser::Chart>();
    chart->Meta = replay.chartMeta;
    return chart;
  };
}

void testMatchingAttemptEnablesHistoricalIr() {
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      validRecord(), cancelled, chartLoader());
  assert(outcome.value.has_value());
  assert(outcome.value->historicalIr.has_value());
  assert(outcome.value->historicalIr->attempt->attemptId == kAttemptId);
  assert(outcome.value->historicalIr->submission->playedAtUnixMillis ==
         1784420645000LL);
  assert(outcome.value->historicalIr->saveOutcome.saved());
}

void testInvalidIntegrityMetadataSuppressesOnlyIr() {
  for (int variant = 0; variant < 4; ++variant) {
    auto record = validRecord();
    if (variant == 0) record.attemptId.reset();
    if (variant == 1) record.attemptFingerprint.reset();
    if (variant == 2) record.attemptFingerprint = std::string(64, 'f');
    if (variant == 3) record.playedAtUnixMillis = 0;
    std::atomic_bool cancelled = false;
    auto outcome = result_recall::BuildChartResult(
        std::move(record), cancelled, chartLoader());
    assert(outcome.value.has_value());
    assert(!outcome.value->historicalIr.has_value());
  }
}

} // namespace

int main() {
  testMatchingAttemptEnablesHistoricalIr();
  testInvalidIntegrityMetadataSuppressesOnlyIr();
  return 0;
}
```

- [ ] **Step 2: Register the test target and verify it fails**

Add `result_recall_builder_tests` to `CMakeLists.txt` with `ResultRecallBuilder.cpp`, `ReplayResultStateBuilder.cpp`, `ResultPersistenceModel.cpp`, `ir/IrSubmission.cpp`, `ir/IrOutboxModels.cpp`, `ScoreProvenance.cpp`, `Uuid.cpp`, and `bms_parser.cpp`; use the same include path, C++23 feature, and common libraries as the existing replay/result test targets. Register it with `asobmashow_register_test`.

Run:

```bash
cmake --build cmake-build-debug --target result_recall_builder_tests -j 6
```

Expected: compilation fails because `ResultRecallBuilder.h` is absent.

- [ ] **Step 3: Add the reconstruction result types**

Create `ResultRecallBuilder.h` with these public types:

```cpp
#pragma once

#include "CoursePlaySession.h"
#include "ResultPersistenceCoordinator.h"
#include "ir/IrSubmission.h"
#include "repositories/ReplayRepository.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace result_recall {

using ReplayChartLoader = std::function<std::unique_ptr<bms_parser::Chart>(
    const ReplayData &, std::atomic_bool &)>;

struct HistoricalIrContext {
  std::shared_ptr<const result_persistence::ChartResultAttempt> attempt;
  std::shared_ptr<const ir::IrSubmission> submission;
  result_persistence::SaveOutcome saveOutcome;
};

struct ChartResult {
  std::unique_ptr<bms_parser::Chart> chart;
  ReplayData replay;
  RhythmState state;
  std::optional<HistoricalIrContext> historicalIr;
};

struct ChartBuildOutcome {
  std::optional<ChartResult> value;
  std::string diagnostic;
};

[[nodiscard]] ChartBuildOutcome
BuildChartResult(ReplayResultRecord record, std::atomic_bool &cancelled,
                 ReplayChartLoader loader = {});

} // namespace result_recall
```

- [ ] **Step 4: Implement view-first reconstruction and fail-closed historical IR**

In `ResultRecallBuilder.cpp`, choose the production parser when no loader is injected, reconstruct state first, and only then validate historical identity:

```cpp
ReplayChartLoader effectiveLoader(ReplayChartLoader loader) {
  if (loader) return loader;
  return [](const ReplayData &replay, std::atomic_bool &cancelled) {
    return play_options::prepareReplayChart(replay.chartMeta.BmsPath, replay,
                                            cancelled);
  };
}

std::optional<HistoricalIrContext>
historicalIrFor(const ReplayResultRecord &record,
                const bms_parser::ChartMeta &meta,
                const RhythmState &state) {
  if (!record.attemptId.has_value() ||
      !record.attemptFingerprint.has_value() ||
      record.attemptFingerprint->empty() ||
      record.playedAtUnixMillis <= 0) {
    return std::nullopt;
  }

  std::string diagnostic;
  auto attempt = result_persistence::makeChartResultAttempt(
      *record.attemptId, meta, state, record.replay.provenance,
      record.replay.chartMeta.LnMode, record.replay, diagnostic);
  if (!attempt.has_value() ||
      attempt->payloadFingerprint != *record.attemptFingerprint) {
    return std::nullopt;
  }
  auto submission = ir::makeIrSubmission(*attempt,
                                         record.playedAtUnixMillis);
  if (!submission.value.has_value()) return std::nullopt;

  auto attemptPtr =
      std::make_shared<const result_persistence::ChartResultAttempt>(
          std::move(*attempt));
  auto submissionPtr = std::make_shared<const ir::IrSubmission>(
      std::move(*submission.value));
  result_persistence::SaveOutcome saved{
      .state = result_persistence::SaveState::Saved,
      .receipt = result_persistence::StageReceipt{
          .attemptId = attemptPtr->attemptId,
          .replayId = record.replay.id,
          .createdAt = record.replay.createdAt,
          .scorePending = false}};
  return HistoricalIrContext{.attempt = std::move(attemptPtr),
                             .submission = std::move(submissionPtr),
                             .saveOutcome = std::move(saved)};
}
```

`BuildChartResult` must return no value only when chart loading/reconstruction fails. A missing ID, malformed timestamp, attempt construction failure, or fingerprint mismatch returns a populated `ChartResult` with `historicalIr == std::nullopt`. Catch parser exceptions and return a sanitized diagnostic without replay contents or credentials.

Add `ResultRecallBuilder.cpp` to the desktop/mobile `main` target in `src/CMakeLists.txt`.

- [ ] **Step 5: Run focused builder and repository tests**

Run:

```bash
cmake --build cmake-build-debug --target result_recall_builder_tests replay_repository_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(result_recall_builder_tests|replay_repository_tests)$'
```

Expected: both tests pass.

- [ ] **Step 6: Commit single-chart reconstruction**

```bash
git add src/ResultRecallBuilder.h src/ResultRecallBuilder.cpp \
  tests/result_recall_builder_tests.cpp src/CMakeLists.txt CMakeLists.txt
git commit -m "feat: reconstruct saved chart results"
```

---

### Task 4: All-or-nothing course result browse sessions

**Files:**
- Modify: `src/ResultRecallBuilder.h`
- Modify: `src/ResultRecallBuilder.cpp`
- Modify: `src/CoursePlaySession.h`
- Modify: `tests/result_recall_builder_tests.cpp`

**Interfaces:**
- Consumes: `CourseReplayData`, the injected/production replay chart loader, course gauge profile, and `BuildResultState`.
- Produces: `result_recall::CourseBuildOutcome` with a fully populated replay-backed `CoursePlaySession` that owns every parsed chart.

- [ ] **Step 1: Add failing course reconstruction tests**

Extend `result_recall_builder_tests.cpp` with a three-stage fixture built from the one-note replay and assert full preparation:

```cpp
void testCourseBuildPreparesEveryStage() {
  CourseReplayData replay;
  replay.id = 9;
  replay.courseName = "Recall Course";
  replay.courseGroupName = "Records";
  replay.constraintJson = "{}";
  replay.gaugeProfile = GaugeProfile::Standard;
  replay.initialGaugeType = GaugeType::Normal;
  for (int index = 0; index < 3; ++index) {
    auto stage = validRecord().replay;
    stage.id = 100 + index;
    stage.chartMeta.Title = "Stage " + std::to_string(index + 1);
    replay.stages.push_back({.replay = std::move(stage),
                             .restMicrosAfterStage = 500000});
  }
  replay.completedCharts = 3;
  replay.totalCharts = 3;
  replay.provenance = replay.stages.back().replay.provenance;

  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildCourseResult(
      replay, cancelled, chartLoader());
  assert(outcome.value.has_value());
  const auto &session = outcome.value->session;
  assert(session->currentIndex == 0);
  assert(session->entries.size() == 3);
  assert(session->completedResults.size() == 3);
  assert(session->ownedResultBrowseCharts.size() == 3);
  assert(session->courseReplayData != nullptr);
  assert(!session->courseReplayPlayback);
}

void testCourseBuildDoesNotPublishPartialSession() {
  CourseReplayData replay;
  replay.courseName = "Broken Course";
  replay.completedCharts = 2;
  replay.totalCharts = 2;
  replay.stages.push_back({.replay = validRecord().replay});
  replay.stages.push_back({.replay = validRecord().replay});
  int calls = 0;
  result_recall::ReplayChartLoader failingLoader =
      [&calls](const ReplayData &stage, std::atomic_bool &) {
        ++calls;
        if (calls == 2) return std::unique_ptr<bms_parser::Chart>{};
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta = stage.chartMeta;
        return chart;
      };
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildCourseResult(
      replay, cancelled, std::move(failingLoader));
  assert(!outcome.value.has_value());
  assert(calls == 2);
}
```

Call both tests from `main`.

- [ ] **Step 2: Run the focused test and verify it fails**

Run:

```bash
cmake --build cmake-build-debug --target result_recall_builder_tests -j 6
```

Expected: compilation fails because course build types and owned charts are absent.

- [ ] **Step 3: Add owned browse charts and the course outcome API**

Add this member to `CoursePlaySession`:

```cpp
std::vector<std::shared_ptr<bms_parser::Chart>> ownedResultBrowseCharts;
```

Add these declarations to `ResultRecallBuilder.h`:

```cpp
struct CourseResult {
  std::shared_ptr<CoursePlaySession> session;
};

struct CourseBuildOutcome {
  std::optional<CourseResult> value;
  std::string diagnostic;
};

[[nodiscard]] CourseBuildOutcome
BuildCourseResult(CourseReplayData replay, std::atomic_bool &cancelled,
                  ReplayChartLoader loader = {});
```

- [ ] **Step 4: Build the course session locally and publish only after every stage succeeds**

Implement `BuildCourseResult` so it first rejects empty, oversized, or inconsistent completed-stage counts. Populate a local `shared_ptr<CoursePlaySession>` with course identity, ruleset snapshot, gauge settings, constraints from `courseConstraintSettingsFromJson`, replay metadata, and `courseReplayData`.

For every stage, load the chart, build its result with the previous `GaugeStateSnapshot`, record its entry/result/provenance, and retain chart ownership:

```cpp
std::optional<GaugeStateSnapshot> carriedGauge;
for (std::size_t index = 0; index < replay.stages.size(); ++index) {
  ReplayData &stageReplay = replay.stages[index].replay;
  auto chart = loader(stageReplay, cancelled);
  if (chart == nullptr || cancelled.load()) {
    return {.diagnostic = "saved course stage is unavailable"};
  }
  RhythmState state = replay_result::BuildResultState(
      *chart, stageReplay, replay.gaugeProfile,
      carriedGauge.has_value() ? &*carriedGauge : nullptr);
  const bool fullCombo = chart->Meta.TotalNotes > 0 &&
                         state.comboBreak == 0 &&
                         state.maxCombo >= chart->Meta.TotalNotes;
  const int reconstructedClear = clear_policy::fullComboRankForPlayback(
      state.getClearTypeRank(), fullCombo, stageReplay.provenance.playback);
  if (state.getScore() != stageReplay.finalScore ||
      state.maxCombo != stageReplay.maxCombo ||
      reconstructedClear != stageReplay.clearType) {
    return {.diagnostic = "saved course stage outcome does not match"};
  }

  session->entries.push_back({.meta = chart->Meta});
  session->completedResults.emplace_back(chart->Meta, state);
  session->recordStageProvenance(index, stageReplay.provenance);
  session->replayStages.push_back(replay.stages[index]);
  carriedGauge = state.gaugeSnapshot();
  session->carriedGauge = carriedGauge;
  session->carriedCombo = state.combo;
  session->maxCombo = std::max(session->maxCombo, state.maxCombo);
  session->ownedResultBrowseCharts.emplace_back(std::move(chart));
}
```

Set `currentIndex = 0`, `courseReplayPlayback = false`, `courseReplaySaved = true`, `courseScoreSaved = true`, and retain a shared copy of the original `CourseReplayData`. Return the session only after the loop completes.

- [ ] **Step 5: Run the builder tests**

Run:

```bash
cmake --build cmake-build-debug --target result_recall_builder_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^result_recall_builder_tests$'
```

Expected: the single-chart and course cases pass, including the no-partial-session failure.

- [ ] **Step 6: Commit course reconstruction**

```bash
git add src/ResultRecallBuilder.h src/ResultRecallBuilder.cpp \
  src/CoursePlaySession.h tests/result_recall_builder_tests.cpp
git commit -m "feat: prepare saved course result sessions"
```

---

### Task 5: Manual saved-course navigation in ResultScene

**Files:**
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Create: `scripts/check_saved_course_result_browse.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: fully prepared `CoursePlaySession::completedResults`, `ownedResultBrowseCharts`, and `courseReplayData` from Task 4.
- Produces: `ResultCourseOptions::savedResultBrowsing` and direct ResultScene-to-ResultScene navigation for stage and aggregate results.

- [ ] **Step 1: Add a failing source-flow audit**

Create `scripts/check_saved_course_result_browse.py` that reads `ResultScene.h` and `ResultScene.cpp` and exits nonzero unless all of these exact contracts are present:

```python
from pathlib import Path
import sys

root = Path(sys.argv[1])
header = (root / "src/scene/ResultScene.h").read_text()
source = (root / "src/scene/ResultScene.cpp").read_text()
required_header = ["bool savedResultBrowsing = false;"]
required_source = [
    "courseOptions.savedResultBrowsing",
    "showSavedCourseStage",
    "session->completedResults[session->currentIndex]",
    ".savedResultBrowsing = true",
]
missing = [token for token in required_header if token not in header]
missing += [token for token in required_source if token not in source]
if missing:
    raise SystemExit("missing saved course result contracts: " +
                     ", ".join(missing))
```

Register it as `saved_course_result_browse_audit` beside the existing Python flow audits in `CMakeLists.txt`.

- [ ] **Step 2: Run the audit and verify it fails**

Run:

```bash
python3 scripts/check_saved_course_result_browse.py .
```

Expected: failure listing the missing browse flag and navigation method.

- [ ] **Step 3: Add the explicit browse mode**

Extend `ResultCourseOptions`:

```cpp
struct ResultCourseOptions {
  ResultCourseMode mode = ResultCourseMode::None;
  std::shared_ptr<CoursePlaySession> session = nullptr;
  bool savedResultBrowsing = false;
};
```

Declare `void showSavedCourseStage();` in `ResultScene`.

Guard `saveCourseScore`, `saveCourseReplay`, and `recordCourseStageRestTime` with `courseOptions.savedResultBrowsing`. In `addCourseButtons`, make the stage back button call `exitResult()` directly when browsing, and skip `buildCourseExitConfirmation()` for browse mode.

- [ ] **Step 4: Navigate prepared stages without gameplay**

Add the saved-result branch before `courseReplayPlayback` in `continueCourse`:

```cpp
if (courseOptions.savedResultBrowsing) {
  if (session->currentIndex + 1 >= session->completedResults.size()) {
    showCourseResult();
    return;
  }
  ++session->currentIndex;
  showSavedCourseStage();
  return;
}
```

Implement the direct stage transition:

```cpp
void ResultScene::showSavedCourseStage() {
  auto session = courseOptions.session;
  if (session == nullptr || session->courseReplayData == nullptr ||
      session->currentIndex >= session->completedResults.size() ||
      session->currentIndex >= session->courseReplayData->stages.size()) {
    exitResult();
    return;
  }
  const auto &result = session->completedResults[session->currentIndex];
  const auto &replay =
      session->courseReplayData->stages[session->currentIndex].replay;
  session->applyReplayStagePlayOptions(replay);
  context.sceneManager->changeScene(
      std::make_unique<ResultScene>(
          context, result.meta, result.state, replay.provenance, nullptr,
          ResultPersistenceOptions{}, nullptr, ResultPracticeOptions{}, false,
          ResultCourseOptions{.mode = ResultCourseMode::Stage,
                              .session = session,
                              .savedResultBrowsing = true}),
      false);
}
```

Propagate `.savedResultBrowsing = courseOptions.savedResultBrowsing` from `showCourseResult`. The existing replay-playback timer remains conditioned only on `courseReplayPlayback`, so browse mode stays manual. The aggregate `Replay` action continues to use `courseReplayData` normally.

- [ ] **Step 5: Run the navigation audit and focused result tests**

Run:

```bash
python3 scripts/check_saved_course_result_browse.py .
cmake --build cmake-build-debug --target result_gauge_history_tests \
  ir_result_presentation_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(saved_course_result_browse_audit|result_gauge_history_tests|ir_result_presentation_tests)$'
```

Expected: all three tests pass.

- [ ] **Step 6: Commit ResultScene browsing**

```bash
git add src/scene/ResultScene.h src/scene/ResultScene.cpp \
  scripts/check_saved_course_result_browse.py CMakeLists.txt
git commit -m "feat: browse saved course results manually"
```

---

### Task 6: Replace Records photo export with View Result

**Files:**
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Create: `scripts/check_records_result_recall_flow.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ReplayRepository::LoadReplayResult`, `ReplayRepository::LoadCourseReplay`, both Task 3/4 builders, and the Task 5 course browse option.
- Produces: the visible `View Result` action and deferred transitions into single-chart or course `ResultScene`.

- [ ] **Step 1: Add the failing Records-flow audit**

Create `scripts/check_records_result_recall_flow.py`:

```python
from pathlib import Path
import sys

root = Path(sys.argv[1])
header = (root / "src/scene/MainMenuScene.h").read_text()
source = (root / "src/scene/MainMenuScene.cpp").read_text()
required = [
    "replayModalResultButton",
    "replayResultRecallInProgress",
    "startReplayResultRecall",
    'makeModalButton("View Result"',
    "LoadReplayResult",
    "BuildChartResult",
    "BuildCourseResult",
    ".savedResultBrowsing = true",
]
combined = header + source
missing = [token for token in required if token not in combined]
forbidden = [
    "replayModalPhotoButton",
    "startReplayImageExport",
    'makeModalButton("Export Photo"',
]
present = [token for token in forbidden if token in combined]
if missing or present:
    raise SystemExit("records result recall contract failure; missing=" +
                     repr(missing) + " forbidden=" + repr(present))
```

Register `records_result_recall_flow_audit` next to the other Python flow audits.

- [ ] **Step 2: Run the audit and verify it fails**

Run:

```bash
python3 scripts/check_records_result_recall_flow.py .
```

Expected: failure listing missing result-action tokens and the old photo-action tokens.

- [ ] **Step 3: Rename the footer action and define its loading guard**

Rename `replayModalPhotoButton`/`Text` to `replayModalResultButton`/`Text`, create it with `View Result`, and replace the photo click listener with:

```cpp
replayModalResultButton->setOnClickListener([this]() {
  if (replayExportInProgress.load() || replayResultRecallInProgress ||
      selectedReplayIsAutoPlay() || !selectedReplaySummary.has_value()) {
    return;
  }
  startReplayResultRecall(replayModalChart, selectedReplaySummary->id);
});
```

Add a `bool replayResultRecallInProgress = false;`, declare `startReplayResultRecall`, and make `refreshReplayModalActions` label the action `Loading...` while active and `View Result` otherwise. Enable it only for a persisted non-Auto-Play selection while list mode is active and no export, watch-options, filter/sort, export-options, or progress operation is active.

Remove `startReplayImageExport`, its `ResultImageExportResult` queue overload, and photo-specific modal completion state. Keep ResultScene's `Export Photo` implementation unchanged and keep video export behavior unchanged.

- [ ] **Step 4: Implement deferred single-chart recall**

Set the guard and loading label, stop preview audio, then use the scene's existing `defer` mechanism so the pointer callback returns before parsing:

```cpp
void MainMenuScene::startReplayResultRecall(const ChartMetaRecord &record,
                                            int replayId) {
  if (replayResultRecallInProgress ||
      replay_autoplay::isAutoPlayReplayId(replayId)) return;
  replayResultRecallInProgress = true;
  refreshReplayModalActions();
  context.jukebox.stop();
  defer([this, record, replayId]() {
    if (!record.courseStart) {
      auto stored = context.replayRepository.LoadReplayResult(
          replayId, replayLoadMetaForRecord(record));
      std::atomic_bool cancelled = false;
      auto recalled = stored.has_value()
          ? result_recall::BuildChartResult(std::move(*stored), cancelled)
          : result_recall::ChartBuildOutcome{};
      if (recalled.value.has_value()) {
        auto result = std::move(*recalled.value);
        ResultPersistenceOptions persistence;
        if (result.historicalIr.has_value()) {
          persistence.attempt = result.historicalIr->attempt;
          persistence.irSubmission = result.historicalIr->submission;
          persistence.outcome = result.historicalIr->saveOutcome;
        }
        const ReplayData replay = result.replay;
        auto chart = std::move(result.chart);
        const bms_parser::ChartMeta meta = chart->Meta;
        context.sceneManager->changeScene(
            std::make_unique<ResultScene>(
                context, meta, result.state, replay.provenance,
                nullptr, std::move(persistence), &replay,
                ResultPracticeOptions{}, false, ResultCourseOptions{},
                profileSelections.pacemakerTarget, std::move(chart)),
            true);
        return false;
      }
      finishReplayResultRecallFailure();
      return false;
    }
    startCourseReplayResultRecall(replayId);
    return false;
  }, 1, true);
}
```

`finishReplayResultRecallFailure` must keep the guard active, set the button text to `Result Unavailable`, log only the sanitized builder diagnostic, and use `defer` to clear the guard and restore `View Result` after 1400 ms without embedding replay events, API keys, or payload JSON. The modal close listener and `hideReplayModal` must also return while the recall guard is active.

- [ ] **Step 5: Implement all-or-nothing course recall and initial stage launch**

Load the entire course, build the session, and transition only after success:

```cpp
void MainMenuScene::startCourseReplayResultRecall(int replayId) {
  auto stored = context.replayRepository.LoadCourseReplay(replayId);
  std::atomic_bool cancelled = false;
  auto recalled = stored.has_value()
      ? result_recall::BuildCourseResult(std::move(*stored), cancelled)
      : result_recall::CourseBuildOutcome{};
  if (!recalled.value.has_value() || recalled.value->session == nullptr ||
      recalled.value->session->completedResults.empty() ||
      recalled.value->session->courseReplayData == nullptr) {
    finishReplayResultRecallFailure();
    return;
  }

  auto session = std::move(recalled.value->session);
  session->currentIndex = 0;
  const auto &result = session->completedResults.front();
  const auto &replay = session->courseReplayData->stages.front().replay;
  session->applyReplayStagePlayOptions(replay);
  context.sceneManager->changeScene(
      std::make_unique<ResultScene>(
          context, result.meta, result.state, replay.provenance, nullptr,
          ResultPersistenceOptions{}, nullptr, ResultPracticeOptions{}, false,
          ResultCourseOptions{.mode = ResultCourseMode::Stage,
                              .session = session,
                              .savedResultBrowsing = true}),
      true);
}
```

- [ ] **Step 6: Run Records, builder, repository, and ResultScene checks**

Run:

```bash
python3 scripts/check_records_result_recall_flow.py .
cmake --build cmake-build-debug --target result_recall_builder_tests \
  replay_repository_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(records_result_recall_flow_audit|saved_course_result_browse_audit|result_recall_builder_tests|replay_repository_tests|ir_result_presentation_tests|result_gauge_history_tests)$'
```

Expected: the flow audits and focused executable tests pass, and `main` links.

- [ ] **Step 7: Commit the Records integration**

```bash
git add src/scene/MainMenuScene.h src/scene/MainMenuScene.cpp \
  scripts/check_records_result_recall_flow.py CMakeLists.txt
git commit -m "feat: reopen saved results from records"
```

---

### Task 7: Regression verification and publication

**Files:**
- Verify only; modify code only if a failing test identifies a defect in Tasks 1-5.

**Interfaces:**
- Consumes: all implementation tasks.
- Produces: a verified branch pushed to the existing remote feature branch.

- [ ] **Step 1: Run formatting and stale-name checks**

Run:

```bash
git diff --check
if rg -n "replayModalPhotoButton|startReplayImageExport|Export Photos" \
  src/scene/MainMenuScene.h src/scene/MainMenuScene.cpp; then exit 1; fi
```

Expected: `git diff --check` is silent and the stale-name search has no matches.

- [ ] **Step 2: Run the complete configured CTest suite**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

Expected: all configured tests pass.

- [ ] **Step 3: Run the repository-prescribed desktop build**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: `main` builds successfully.

- [ ] **Step 4: Review the final diff for scope and credential safety**

Run:

```bash
git diff --stat 6b8a599..HEAD
git diff 6b8a599..HEAD -- src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryRecords.cpp src/ResultRecallBuilder.h \
  src/ResultRecallBuilder.cpp src/CoursePlaySession.h \
  src/scene/ResultScene.h src/scene/ResultScene.cpp \
  src/scene/MainMenuScene.h src/scene/MainMenuScene.cpp
rg -n "apiKey|credential|Authorization" src/ResultRecallBuilder.* \
  src/repositories/ReplayRepositoryRecords.cpp
```

Expected: the diff contains only result-recall work, and the credential search adds no credential storage or copying to recalled/outbox models.

- [ ] **Step 5: Push the feature branch**

Run:

```bash
git push origin feature/bokutachi-ir
```

Expected: the remote branch advances to the verified local HEAD and updates the existing pull request.
