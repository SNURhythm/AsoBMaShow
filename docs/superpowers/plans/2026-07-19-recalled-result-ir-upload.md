# Recalled Result IR Upload Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore Bokutachi controls on fingerprint-valid recalled results and let an eligible Records-row badge queue or retry that saved score without leaving the modal.

**Architecture:** Reconstruct `ReplayData::chartMeta` from the complete replay-configured parsed chart before validating the historical attempt. Add a provider-neutral saved-result upload action that decides between enqueue, retry, and no-op from the authoritative outbox row; expose it through an interactive virtualized badge coordinated by `MainMenuScene`.

**Tech Stack:** C++23, SDL2 pointer events, Yoga views, SQLite-backed `ReplayRepository`, existing IR driver registry and durable `IrSubmissionService`, CMake/CTest, Python source-contract audits.

## Global Constraints

- Do not relax, skip, or rewrite the stored attempt fingerprint check.
- Only eligible, verified single-chart LR2 results may create new Bokutachi work.
- Auto Play, course, practice, assisted, modified, Beatoraja, and legacy-unverified results remain ineligible.
- The Records modal stays open when the amber badge is used.
- Existing outbox rows are authoritative; never replace an active or completed payload.
- API keys remain resolved only by `IrSubmissionService` at send time and must never enter replay models, action results, diagnostics, logs, or outbox payload columns.
- The amber marker remains visible until the authoritative outbox state is `Succeeded`.
- All diagnostics shown by the modal are sanitized and bounded.
- Follow red-green TDD for every production behavior.
- Use `scripts/ios_firebase_deploy.sh` only for an explicitly requested deployment; this plan performs local builds only.

---

## File Map

- Modify `src/ResultRecallBuilder.cpp`: restore the complete parsed chart metadata before historical attempt reconstruction.
- Modify `tests/result_recall_builder_tests.cpp`: reproduce database-shaped sparse metadata and retain the fingerprint fail-closed case.
- Create `src/ir/IrSavedResultUpload.h`: provider-neutral action result, dependency boundary, and public execution function.
- Create `src/ir/IrSavedResultUpload.cpp`: authoritative outbox-state branching for enqueue/retry/no-op.
- Create `tests/ir_saved_result_upload_tests.cpp`: unit coverage for every outbox state and failure outcome.
- Modify `src/ir/CMakeLists.txt` and root `CMakeLists.txt`: compile and register the new action and focused tests.
- Modify `src/view/ReplaySummaryListView.h`: turn `IR ↑` into a virtualized child button with rebound identity and busy state.
- Create `tests/replay_summary_list_view_tests.cpp`: actual SDL click routing and recycled-binding coverage.
- Modify `src/scene/MainMenuScene.h`: Records-modal upload state and coordinator methods.
- Modify `src/scene/MainMenuScene.cpp`: deferred preparation, action execution, feedback, guards, and row refresh.
- Modify `scripts/check_records_result_recall_flow.py`: assert the new badge-to-outbox wiring and cleanup contract.

---

### Task 1: Restore Fingerprint-Complete Recalled Chart Metadata

**Files:**
- Modify: `tests/result_recall_builder_tests.cpp`
- Modify: `src/ResultRecallBuilder.cpp:68-88`

**Interfaces:**
- Consumes: `result_recall::BuildChartResult(ReplayResultRecord, std::atomic_bool &, ReplayChartLoader)`.
- Produces: a `ChartResult::historicalIr` value when the stored replay is database-shaped but the reparsed chart exactly matches the original attempt.

- [ ] **Step 1: Add a database-shaped metadata regression fixture**

Extend `validRecord()` so its original fingerprint covers non-database chart metadata such as `Folder`, `SubArtist`, `Bpm`, `Genre`, `Rank`, `Total`, `HasTotal`, `PlayLength`, `Banner`, and `StageFile`. Add a helper that preserves the fields stored by `replays` but clears the rest:

```cpp
bms_parser::ChartMeta databaseShapedMeta(const bms_parser::ChartMeta &full) {
  bms_parser::ChartMeta sparse;
  sparse.BmsPath = std::filesystem::path("stored") / "recall.bms";
  sparse.MD5 = full.MD5;
  sparse.SHA256 = full.SHA256;
  sparse.Title = full.Title;
  sparse.Artist = full.Artist;
  sparse.KeyMode = full.KeyMode;
  sparse.TotalNotes = full.TotalNotes;
  sparse.LnMode = full.LnMode;
  return sparse;
}
```

Add the regression:

```cpp
void testParsedChartMetadataRestoresHistoricalIr() {
  ReplayResultRecord record = validRecord();
  const bms_parser::ChartMeta original = record.replay.chartMeta;
  record.replay.chartMeta = databaseShapedMeta(original);
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      std::move(record), cancelled,
      [original](const ReplayData &, std::atomic_bool &) {
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta = original;
        return chart;
      });
  assert(outcome.value.has_value());
  assert(outcome.value->historicalIr.has_value());
  assert(outcome.value->replay.chartMeta.SubArtist == original.SubArtist);
}
```

- [ ] **Step 2: Add the parsed-chart mismatch safety regression**

```cpp
void testChangedParsedChartStillSuppressesHistoricalIr() {
  ReplayResultRecord record = validRecord();
  bms_parser::ChartMeta changed = record.replay.chartMeta;
  record.replay.chartMeta = databaseShapedMeta(changed);
  changed.Genre += " changed";
  std::atomic_bool cancelled = false;
  auto outcome = result_recall::BuildChartResult(
      std::move(record), cancelled,
      [changed](const ReplayData &, std::atomic_bool &) {
        auto chart = std::make_unique<bms_parser::Chart>();
        chart->Meta = changed;
        return chart;
      });
  assert(outcome.value.has_value());
  assert(!outcome.value->historicalIr.has_value());
}
```

Call both tests from `main()`.

- [ ] **Step 3: Run the focused test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target result_recall_builder_tests -j 6
./cmake-build-debug/result_recall_builder_tests
```

Expected: `testParsedChartMetadataRestoresHistoricalIr` aborts because `historicalIr` is absent; the changed-chart safety case already remains absent.

- [ ] **Step 4: Restore the complete parsed metadata**

In `BuildChartResult`, replace the path-only copy with the full parsed metadata before rebuilding state:

```cpp
record.replay.chartMeta = chart->Meta;
RhythmState state = replay_result::BuildResultState(*chart, record.replay);
```

Do not alter `historicalIrFor`, fingerprint calculation, or mismatch behavior. Leave course reconstruction unchanged unless a focused course test proves that it requires the same full snapshot for an existing invariant.

- [ ] **Step 5: Run the focused test and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target result_recall_builder_tests -j 6
./cmake-build-debug/result_recall_builder_tests
git diff --check
```

Expected: exit 0 with both restoration and mismatch cases passing.

- [ ] **Step 6: Commit the reconstruction fix**

```bash
git add src/ResultRecallBuilder.cpp tests/result_recall_builder_tests.cpp
git commit -m "fix: restore IR context for recalled results"
```

---

### Task 2: Add a Provider-Neutral Saved-Result Upload Action

**Files:**
- Create: `src/ir/IrSavedResultUpload.h`
- Create: `src/ir/IrSavedResultUpload.cpp`
- Create: `tests/ir_saved_result_upload_tests.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `IrSubmission`, `IrOutboxReadOutcome`, `BuildDraftOutcome`, `IrOutboxInsertOutcome`, and `IrOutboxMutationOutcome`.
- Produces:

```cpp
enum class IrSavedResultUploadState {
  Queued,
  RetryQueued,
  AlreadyActive,
  AlreadySubmitted,
  Failed,
};

struct IrSavedResultUploadResult {
  IrSavedResultUploadState state = IrSavedResultUploadState::Failed;
  bool accepted = false;
  std::string message;
};

struct IrSavedResultUploadDependencies {
  std::function<IrOutboxReadOutcome(std::string_view providerId,
                                    std::string_view attemptId)> loadOutbox;
  std::function<BuildDraftOutcome(const IrSubmission &)> buildDraft;
  std::function<IrOutboxInsertOutcome(const IrOutboxDraft &)> enqueue;
  std::function<IrOutboxMutationOutcome(std::int64_t rowId)> retry;
};

[[nodiscard]] IrSavedResultUploadResult executeIrSavedResultUpload(
    std::string_view providerId, const IrSubmission &submission,
    const IrSavedResultUploadDependencies &dependencies) noexcept;
```

- [ ] **Step 1: Write failing action-policy tests**

Create a fixture that records build/enqueue/retry call counts and returns configurable outcomes. Add separate tests proving:

```cpp
void testAbsentRowBuildsAndEnqueues() {
  Fixture fixture;
  fixture.read.status = ir::IrOutboxReadStatus::NotFound;
  const auto result = fixture.execute();
  REQUIRE(result.state == ir::IrSavedResultUploadState::Queued);
  REQUIRE(result.accepted);
  REQUIRE(fixture.buildCalls == 1);
  REQUIRE(fixture.enqueueCalls == 1);
  REQUIRE(fixture.retryCalls == 0);
}

void testRetryableRowsRetryWithoutRebuilding() {
  for (const auto state : {ir::IrOutboxState::Pending,
                           ir::IrOutboxState::AwaitingRemoteResult,
                           ir::IrOutboxState::BlockedConfiguration,
                           ir::IrOutboxState::FailedPermanent}) {
    Fixture fixture;
    fixture.setExisting(state, 42);
    const auto result = fixture.execute();
    REQUIRE(result.state == ir::IrSavedResultUploadState::RetryQueued);
    REQUIRE(fixture.retryRow == 42);
    REQUIRE(fixture.buildCalls == 0);
    REQUIRE(fixture.enqueueCalls == 0);
  }
}
```

Also cover `Uploading` as `AlreadyActive` with no mutation, `Succeeded` as `AlreadySubmitted`, draft rejection, malformed/storage/integrity read outcomes, enqueue rejection, retry rejection, and thrown dependency callbacks mapping to sanitized `Failed` without exceptions escaping.

- [ ] **Step 2: Register the focused test target and verify RED**

Add `ir_saved_result_upload_tests` to root `CMakeLists.txt` with the new test,
`src/ir/IrOutboxModels.cpp`, `src/ir/IrSubmission.cpp`,
`src/ScoreProvenance.cpp`, and `src/Uuid.cpp`, then add it to the registered
test list. Do not list the not-yet-created implementation source in this red
target.

Run:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target ir_saved_result_upload_tests -j 6
```

Expected: compilation fails because `ir/IrSavedResultUpload.h` does not exist.

- [ ] **Step 3: Implement minimal authoritative branching**

Create the header with the exact interfaces declared above. Create
`IrSavedResultUpload.cpp`, add it to the focused test target, and implement
`executeIrSavedResultUpload` with this order:

```cpp
const IrOutboxReadOutcome existing =
    dependencies.loadOutbox(providerId, submission.attemptId);
if (existing.status == IrOutboxReadStatus::Found && existing.entry) {
  switch (existing.entry->state) {
  case IrOutboxState::Uploading:
    return active("Submission already in progress.");
  case IrOutboxState::Succeeded:
    return submitted("Already submitted.");
  case IrOutboxState::Pending:
  case IrOutboxState::AwaitingRemoteResult:
  case IrOutboxState::BlockedConfiguration:
  case IrOutboxState::FailedPermanent:
    return fromRetry(dependencies.retry(existing.entry->id));
  }
}
if (existing.status != IrOutboxReadStatus::NotFound) {
  return failed(existing.diagnostic.empty()
                    ? "The saved submission state is unavailable."
                    : existing.diagnostic);
}
const BuildDraftOutcome built = dependencies.buildDraft(submission);
if (built.status != BuildDraftStatus::Built || !built.draft) {
  return failed(built.diagnostic.empty()
                    ? "This saved result cannot be submitted."
                    : built.diagnostic);
}
return fromEnqueue(dependencies.enqueue(*built.draft));
```

Treat both `Inserted` and race-safe `AlreadyExists` enqueue outcomes as accepted `Queued`. Treat only `Updated` retry as accepted `RetryQueued`. Wrap the function body in `try/catch (...)` and return a fixed diagnostic on unexpected exceptions. Pass every external diagnostic through `sanitizeDiagnostic`.

- [ ] **Step 4: Add the new source to the application target**

Add `IrSavedResultUpload.cpp` to `src/ir/CMakeLists.txt`. Do not add credential-store or HTTP dependencies to the action.

- [ ] **Step 5: Run the focused test and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target ir_saved_result_upload_tests -j 6
./cmake-build-debug/ir_saved_result_upload_tests
git diff --check
```

Expected: all action-policy cases pass and no dependency is called for active or succeeded rows.

- [ ] **Step 6: Commit the action policy**

```bash
git add CMakeLists.txt src/ir/CMakeLists.txt src/ir/IrSavedResultUpload.h \
  src/ir/IrSavedResultUpload.cpp tests/ir_saved_result_upload_tests.cpp
git commit -m "feat: add saved result IR upload action"
```

---

### Task 3: Make the Virtualized Amber Marker Interactive

**Files:**
- Modify: `src/view/ReplaySummaryListView.h`
- Create: `tests/replay_summary_list_view_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ReplaySummary::id` and `ReplaySummary::irUploadPending`.
- Produces:

```cpp
std::function<void(const ReplaySummary &)> onIrUploadRequested;
void setIrUploadInProgress(std::optional<int> replayId);
```

- [ ] **Step 1: Write an actual pointer-routing regression**

Create `replay_summary_list_view_tests.cpp` with the rendering globals used by `button_enabled_tests`. Construct a `ReplaySummaryListView`, bind one eligible row, lay it out, find the named child button, and send real mouse down/up events through the recycler:

```cpp
ReplaySummaryListView list;
list.setSize(700, 100);
ReplaySummary eligible;
eligible.id = 41;
eligible.irUploadPending = true;
int requestedId = 0;
int selectionChanges = 0;
list.onIrUploadRequested = [&](const ReplaySummary &summary) {
  requestedId = summary.id;
};
list.onSelectionChanged = [&](int) { ++selectionChanges; };
list.setReplaySummaries({eligible});
list.applyYogaLayout();
auto *badge = list.getViewByIndex(0)->findViewByName("irUploadBadge");
REQUIRE(badge != nullptr);
clickThrough(list, badge->getX() + 2, badge->getY() + 2);
REQUIRE(requestedId == 41);
REQUIRE(selectionChanges == 0);
REQUIRE(list.selectedReplayIndex() == -1);
```

Add cases proving an ineligible hidden badge emits nothing, busy state emits nothing and displays `IR ...`, and rebinding the same item view from replay 41 to replay 52 emits 52 rather than retaining 41.

- [ ] **Step 2: Register the test and verify RED**

Create a `replay_summary_list_view_tests` CMake target with:

```cmake
tests/replay_summary_list_view_tests.cpp
src/rendering/Color.cpp
src/rendering/common.cpp
src/rendering/UniformCache.cpp
src/view/Button.cpp
src/view/TextView.cpp
src/view/View.cpp
src/bms_parser.cpp
src/scene/play/GameplayGaugeRules.cpp
```

Link `${COMMON_LIBS} bgfx yogacore`, register the target, then run:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target replay_summary_list_view_tests -j 6
```

Expected: compilation fails because `onIrUploadRequested`, `setIrUploadInProgress`, and the named badge button do not exist.

- [ ] **Step 3: Replace the passive text marker with a button**

In `ReplaySummaryListItemView`:

- Replace `TextView *irBadge` with `Button *irBadge` plus `TextView *irBadgeText`.
- Name the button `irUploadBadge`.
- Store the latest `ReplaySummary currentSummary` on every `setSummary` call.
- Install the button listener once; it reads `currentSummary` at click time and invokes the latest handler only when `currentSummary.irUploadPending` and the row is not busy.
- Keep the existing amber background, text contrast, 54-by-28 geometry, corner radius, visibility, and collapsed width.
- Make `setSummary(const ReplaySummary &, bool uploadBusy)` select `IR ...` and disable the button when busy, otherwise `IR ↑`.

In `ReplaySummaryListView`, capture `this` in `onCreateView`, forward item actions through the public `onIrUploadRequested`, store `std::optional<int> irUploadInProgress`, and call `rebindVisibleItems()` when it changes.

- [ ] **Step 4: Run the focused test and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target replay_summary_list_view_tests -j 6
./cmake-build-debug/replay_summary_list_view_tests
git diff --check
```

Expected: the child button consumes the click, the recycler does not select the row, busy state is disabled, and rebound identity is current.

- [ ] **Step 5: Commit the interactive marker**

```bash
git add CMakeLists.txt src/view/ReplaySummaryListView.h \
  tests/replay_summary_list_view_tests.cpp
git commit -m "feat: make replay IR marker interactive"
```

---

### Task 4: Coordinate Direct Upload Inside the Records Modal

**Files:**
- Modify: `scripts/check_records_result_recall_flow.py`
- Modify: `src/scene/MainMenuScene.h:360-520,740-790`
- Modify: `src/scene/MainMenuScene.cpp:2400-2650,7680-8750,9600-9760,10080-10220`

**Interfaces:**
- Consumes: `ReplaySummaryListView::onIrUploadRequested`, `result_recall::BuildChartResult`, and `executeIrSavedResultUpload`.
- Produces these `MainMenuScene` methods/state:

```cpp
void startReplayIrUpload(const ChartMetaRecord &record,
                         ReplaySummary summary);
void finishReplayIrUpload(int replayId, std::string message);
void refreshReplayIrMarker(int replayId);
bool replayIrUploadInProgress = false;
std::optional<int> replayIrUploadReplayId;
```

- [ ] **Step 1: Strengthen the Records flow audit and verify RED**

Extend `scripts/check_records_result_recall_flow.py` to require:

```python
required += [
    "onIrUploadRequested",
    "startReplayIrUpload",
    "finishReplayIrUpload",
    "refreshReplayIrMarker",
    "replayIrUploadInProgress",
    "executeIrSavedResultUpload",
    "setIrUploadInProgress",
]
```

Extract the `startReplayIrUpload` source region and require `LoadReplayResult`, `BuildChartResult`, `historicalIr`, `loadOutbox`, `buildDraft`, `enqueueManual`, and `retry`. Require cleanup to reset the busy flag and optional replay ID.

Run:

```bash
python3 scripts/check_records_result_recall_flow.py .
```

Expected: failure listing the missing upload wiring tokens.

- [ ] **Step 2: Add modal state and bind the badge callback**

Initialize and reset `replayIrUploadInProgress` and `replayIrUploadReplayId` with the other replay-modal state. After constructing `replayListView`, bind:

```cpp
replayListView->onIrUploadRequested = [this](const ReplaySummary &summary) {
  startReplayIrUpload(replayModalChart, summary);
};
```

Include `replayIrUploadInProgress` in every existing replay-modal operation guard: close, filter, watch, G-BATTLE, View Result, Export Video, `hideReplayModal`, and `refreshReplayModalActions`. This prevents overlapping parse/export/scene operations.

- [ ] **Step 3: Implement deferred preparation and action execution**

At the start of `startReplayIrUpload`, reject Auto Play, course, non-marker,
unavailable-service, disabled-provider, missing-driver, read-only-driver,
non-submitting-driver, and already-busy cases. Set the busy ID, show
`Preparing IR...`, call `setIrUploadInProgress(summary.id)`, cancel preview
loading, and use the existing one-tick deferred pattern.

Inside the deferred callback:

```cpp
auto stored = context.replayRepository.LoadReplayResult(
    summary.id, replayLoadMetaForRecord(record));
std::atomic_bool cancelled = false;
auto recalled = stored.has_value()
                    ? result_recall::BuildChartResult(std::move(*stored),
                                                       cancelled)
                    : result_recall::ChartBuildOutcome{};
if (!recalled.value || !recalled.value->historicalIr) {
  finishReplayIrUpload(summary.id,
                       "This saved result could not be verified for IR.");
  return true;
}

const auto &submission = *recalled.value->historicalIr->submission;
const ir::IrSavedResultUploadDependencies dependencies{
    .loadOutbox = [this](std::string_view provider,
                         std::string_view attempt) {
      return context.replayRepository.LoadIrOutbox(provider, attempt);
    },
    .buildDraft = [this](const ir::IrSubmission &value) {
      return context.irDrivers.buildDraft(ir::kTachiProviderId, value);
    },
    .enqueue = [this](const ir::IrOutboxDraft &draft) {
      return context.irSubmissionService->enqueueManual(draft);
    },
    .retry = [this](std::int64_t rowId) {
      return context.irSubmissionService->retry(rowId);
    },
};
const auto action = ir::executeIrSavedResultUpload(
    ir::kTachiProviderId, submission, dependencies);
finishReplayIrUpload(summary.id, action.message);
return true;
```

Sanitize every fallback message and never log the submission payload.

- [ ] **Step 4: Refresh the affected marker without resetting the modal**

Implement `refreshReplayIrMarker` by locating the replay in `replaySummaries`, loading its exact outbox row by attempt ID, updating `requestedIrOutboxState`, recalculating `irUploadPending` with `shouldShowReplayUploadMarker`, and reapplying filters with the replay ID as the preferred selection.

Preserve `replayListView->scrollOffset` across `applyReplayRecordFilters(replayId)`, restore it afterward, then call `rebindVisibleItems()`. If the exact outbox read fails, leave the existing marker state unchanged.

In `finishReplayIrUpload`, clear the busy state on all paths, clear the list's busy ID, refresh the affected marker, show the bounded action message in `replayModalTitleText`, refresh action availability, and defer restoring the title to `Records` after 1400 ms if the list mode is still visible.

- [ ] **Step 5: Run audit, focused tests, and compile checks**

Run:

```bash
python3 scripts/check_records_result_recall_flow.py .
cmake --build cmake-build-debug --target result_recall_builder_tests \
  ir_saved_result_upload_tests replay_summary_list_view_tests main -j 6
./cmake-build-debug/result_recall_builder_tests
./cmake-build-debug/ir_saved_result_upload_tests
./cmake-build-debug/replay_summary_list_view_tests
git diff --check
```

Expected: audit exits 0, all three focused tests pass, and `main` links successfully.

- [ ] **Step 6: Commit the Records integration**

```bash
git add scripts/check_records_result_recall_flow.py \
  src/scene/MainMenuScene.h src/scene/MainMenuScene.cpp
git commit -m "feat: upload saved scores from Records"
```

---

### Task 5: Verify the Complete Change and Push

**Files:**
- Verify only; modify production files only if a failing test exposes a defect.

**Interfaces:**
- Consumes: all prior task commits.
- Produces: a clean, pushed `feature/bokutachi-ir` branch.

- [ ] **Step 1: Run focused regression targets**

```bash
cmake --build cmake-build-debug --target result_recall_builder_tests \
  ir_saved_result_upload_tests replay_summary_list_view_tests \
  ir_result_presentation_tests ir_submission_service_tests \
  replay_repository_tests -j 6
./cmake-build-debug/result_recall_builder_tests
./cmake-build-debug/ir_saved_result_upload_tests
./cmake-build-debug/replay_summary_list_view_tests
./cmake-build-debug/ir_result_presentation_tests
./cmake-build-debug/ir_submission_service_tests
./cmake-build-debug/replay_repository_tests
```

Expected: every executable exits 0.

- [ ] **Step 2: Run source contracts and formatting validation**

```bash
python3 scripts/check_records_result_recall_flow.py .
python3 scripts/check_records_ir_marker.py .
git diff --check
```

Expected: both audits and whitespace validation exit 0.

- [ ] **Step 3: Run the complete configured suite**

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

Expected: 100% pass with zero failed tests.

- [ ] **Step 4: Build the required desktop target**

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: `main` links successfully; existing third-party bgfx warnings may remain, but no new project warning or error is introduced.

- [ ] **Step 5: Audit the final diff and repository state**

```bash
git status --short
git log --oneline --decorate origin/feature/bokutachi-ir..HEAD
git diff --stat origin/feature/bokutachi-ir..HEAD
git diff --check origin/feature/bokutachi-ir..HEAD
```

Expected: no uncommitted files, only the approved design/plan and implementation commits are ahead, and diff validation is clean.

- [ ] **Step 6: Push and verify the remote head**

```bash
git push origin feature/bokutachi-ir
git status -sb
git rev-parse HEAD
git ls-remote --heads origin feature/bokutachi-ir
```

Expected: local and remote hashes match and the branch reports no divergence.
