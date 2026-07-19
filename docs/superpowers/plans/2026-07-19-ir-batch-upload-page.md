# IR Batch Upload Page Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Song Select `IR Uploads` page that lists every actionable saved score, supports individual/select-all selection, queues verified attempts together, and sends the fewest valid Tachi Batch Manual requests.

**Architecture:** A dedicated scene consumes explicit replay-candidate and chart-metadata repository outcomes, then projects them through the same semantic eligibility resolver as Records. Local result reconstruction produces independent durable outbox drafts in one batch mutation; the submission worker keeps per-attempt state while claiming, transporting, polling, and completing compatible entries as atomic provider batches.

**Tech Stack:** C++23, SDL2, Yoga/bgfx views, SQLite replay/chart repositories, existing `IrSubmissionService` and Tachi Direct/Batch Manual integration, CMake/CTest.

## Global Constraints

- The page includes semantic IR states `Eligible` and `Failed` only.
- Every eligible attempt is a separate row, including multiple attempts for one chart.
- Courses, Auto Play, remote-only scores, practice, assisted, modified, legacy-unverified, queued, active, blocked, and uploaded results remain excluded.
- Exact attempt fingerprint and Bokutachi eligibility checks remain mandatory.
- Network delivery batches compatible scores; it never sends one request per selected score when multiple scores fit one provider document.
- Tachi documents are separated by 7K/14K playtype and split further only for the 64 KiB payload bound or 64-entry worker bound.
- One outbox row remains the durable state and receipt identity for one attempt.
- API keys remain runtime-only and must not enter payloads, outbox rows, receipts, diagnostics, or logs.
- Provider-disabled and missing-key states expose a direct `Open IR Settings` action.
- New normal C++ sources are discovered by the iOS synchronized Xcode group; do not add them to `membershipExceptions`.
- Do not deploy to Firebase while verifying this feature.

---

## File Structure

- Create `src/ir/IrReplayRecordState.h/.cpp`: shared Records/page semantic IR state resolution.
- Create `src/ir/IrUploadCandidates.h/.cpp`: candidate presentation model, projection, selection helpers, and bounded diagnostics.
- Create `src/ir/IrSavedResultBatchUpload.h/.cpp`: pure build-and-enqueue orchestration for reconstructed submissions.
- Create `src/view/IrUploadCandidateListView.h/.cpp`: virtualized chart-style selectable rows.
- Create `src/scene/IrUploadsScene.h/.cpp`: page lifecycle, repository loading, progress mailbox, cancellation, and navigation.
- Modify `src/repositories/ReplayRepository.h`, `ReplayRepositoryRecords.cpp`, and `ReplayRepositoryIrOutbox.cpp`: candidate reads and atomic outbox batch mutations.
- Modify `src/repositories/ChartRepository.h`, `ChartRepositoryInternal.h`, and `ChartRepositoryQueries.cpp`: explicit bulk chart-path hydration.
- Modify `src/ir/IrOutboxModels.h/.cpp`, `IrDriver.h/.cpp`, `IrSubmissionService.h/.cpp`, and Tachi driver/parser files: generic grouped delivery plus Tachi batch planning and parsing.
- Modify `src/scene/MainMenuScene.h/.cpp`, `SettingsScene.h/.cpp`, `SettingsSceneLayout.cpp`, and scene/view/IR CMake files: entry, direct settings destination, and source registration.
- Add focused tests in `tests/ir_upload_candidates_tests.cpp`, `tests/ir_saved_result_batch_upload_tests.cpp`, and `tests/ir_upload_candidate_list_view_tests.cpp`; extend the existing repository, Tachi driver, and service tests.
- Create `scripts/check_ir_uploads_flow.py`: source-level integration audit for the header entry, dedicated scene, settings destination, and batched service call.

---

### Task 1: Share Records Eligibility and Project Upload Candidates

**Files:**
- Create: `src/ir/IrReplayRecordState.h`
- Create: `src/ir/IrReplayRecordState.cpp`
- Create: `src/ir/IrUploadCandidates.h`
- Create: `src/ir/IrUploadCandidates.cpp`
- Create: `tests/ir_upload_candidates_tests.cpp`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `ReplaySummary`, `ChartMetaRecord`, `ir::IrRecordActivity`, and `ir::tachi::isReplayEligibleForBokutachi`.
- Produces:

```cpp
namespace ir {
void resolveReplayIrRecordState(
    ReplaySummary &summary,
    IrRecordActivity activity = IrRecordActivity::None) noexcept;

struct IrUploadCandidate {
  ReplaySummary replay;
  ChartMetaRecord chart;
  IrRecordState state = IrRecordState::Hidden;

  [[nodiscard]] int replayId() const noexcept { return replay.id; }
};

struct IrUploadCandidateProjection {
  std::vector<IrUploadCandidate> candidates;
  std::size_t omittedRows = 0;
  std::string diagnostic;
};

[[nodiscard]] IrUploadCandidateProjection projectIrUploadCandidates(
    std::span<const ReplaySummary> replays,
    std::span<const ChartMetaRecord> charts) noexcept;

void intersectIrUploadSelection(
    std::unordered_set<int> &selectedReplayIds,
    std::span<const IrUploadCandidate> candidates);
}
```

- [ ] **Step 1: Write failing projection and selection tests**

Add cases that construct complete 7K metadata and verified LR2 provenance using the existing test fixture helpers. Exercise every semantic state, duplicate attempts for one chart, missing chart paths, path deduplication, and selection intersection:

```cpp
const std::array replays{
    eligible, failed, queued, uploaded, hidden, duplicateEligible};
const std::array charts{chart};
const auto projected = ir::projectIrUploadCandidates(replays, charts);
expect(projected.candidates.size() == 3,
       "only eligible, failed, and duplicate eligible attempts remain");
expect(projected.candidates[0].replayId() == eligible.id &&
           projected.candidates[1].replayId() == failed.id &&
           projected.candidates[2].replayId() == duplicateEligible.id,
       "projection preserves newest-first replay order");

std::unordered_set<int> selected{eligible.id, queued.id, 99999};
ir::intersectIrUploadSelection(selected, projected.candidates);
expect(selected == std::unordered_set<int>{eligible.id},
       "refresh retains only still-published replay IDs");
```

Assert projection hydrates `replay.chartMeta` from the chart record, calculates `maxScore = TotalNotes * 2`, reruns the canonical resolver, reports one bounded omission diagnostic, and never promotes pre-labelled but ineligible rows.

- [ ] **Step 2: Register and run the new test to verify RED**

Add `ir_upload_candidates_tests` to root `CMakeLists.txt` with these production sources: `IrReplayRecordState.cpp`, `IrUploadCandidates.cpp`, `IrReceiptModels.cpp`, `IrProfileSettings.cpp`, `TachiBatchManual.cpp`, `FileChecksum.cpp`, `ScoreProvenance.cpp`, gameplay gauge/judge rules, `Judge.cpp`, `Uuid.cpp`, and `ChartStorageIdentity.cpp`. Add it to the registered test target list.

Run:

```bash
cmake --build cmake-build-debug --target ir_upload_candidates_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^ir_upload_candidates_tests$'
```

Expected: compilation fails because the new headers/functions do not exist.

- [ ] **Step 3: Implement the shared state resolver**

Move the anonymous `resolveReplayIrRecordState` logic out of `MainMenuScene.cpp` without changing it:

```cpp
void resolveReplayIrRecordState(ReplaySummary &summary,
                                IrRecordActivity activity) noexcept {
  summary.irSubmissionEligible =
      summary.attemptId.has_value() && summary.chartMeta.has_value() &&
      summary.provenance != nullptr &&
      tachi::isReplayEligibleForBokutachi(
          *summary.attemptId, summary.hasCanonicalAttemptFingerprint,
          *summary.chartMeta, *summary.provenance);
  summary.irRecordState = resolveIrRecordState({
      .eligible = summary.irSubmissionEligible,
      .hasReceipt = summary.hasIrReceipt,
      .outboxState = summary.requestedIrOutboxState,
      .activity = activity,
  });
}
```

Include the new header in `MainMenuScene.cpp` and replace both calls with `ir::resolveReplayIrRecordState`.

- [ ] **Step 4: Implement fail-closed candidate projection**

Build a path-keyed map using `chart_storage_identity::StoredPathText`. For each replay, require positive replay ID, non-course/non-Auto Play, a stored chart path, exactly one hydrated chart, and consistent MD5/SHA-256 when both stored and hydrated values are present. Copy the full chart metadata into the replay, resolve state again, and publish only `Eligible` or `Failed`:

```cpp
hydrated.chartMeta = chart.meta;
if (chart.meta.TotalNotes <= 0 ||
    chart.meta.TotalNotes > std::numeric_limits<int>::max() / 2) {
  ++result.omittedRows;
  continue;
}
hydrated.maxScore = chart.meta.TotalNotes * 2;
resolveReplayIrRecordState(hydrated);
if (hydrated.irRecordState != IrRecordState::Eligible &&
    hydrated.irRecordState != IrRecordState::Failed) {
  continue;
}
const IrRecordState state = hydrated.irRecordState;
result.candidates.push_back({.replay = std::move(hydrated),
                             .chart = chart,
                             .state = state});
```

Sanitize the aggregate diagnostic and never include a filesystem path or provenance payload.

- [ ] **Step 5: Run tests and commit**

Run:

```bash
cmake --build cmake-build-debug --target ir_upload_candidates_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(ir_upload_candidates_tests|result_record_summary_tests|result_record_list_view_tests)$'
git diff --check
git add src/ir/IrReplayRecordState.* src/ir/IrUploadCandidates.* \
  src/scene/MainMenuScene.cpp src/ir/CMakeLists.txt \
  tests/ir_upload_candidates_tests.cpp CMakeLists.txt
git commit -m "refactor: share IR upload candidate semantics"
```

Expected: all selected tests pass.

---

### Task 2: Compose and Parse Provider-Native Tachi Batches

**Files:**
- Modify: `src/ir/IrDriver.h`
- Modify: `src/ir/IrDriver.cpp`
- Modify: `src/ir/tachi/TachiBatchManual.h`
- Modify: `src/ir/tachi/TachiBatchManual.cpp`
- Modify: `src/ir/tachi/TachiResponseParser.cpp`
- Modify: `src/ir/tachi/TachiDriver.h`
- Modify: `src/ir/tachi/TachiDriver.cpp`
- Modify: `tests/ir_driver_tests.cpp`
- Modify: `tests/tachi_batch_manual_tests.cpp`
- Modify: `tests/tachi_driver_tests.cpp`

**Interfaces:**
- Consumes: due `IrOutboxEntry` values carrying validated one-score payloads and ruleset proofs.
- Produces:

```cpp
enum class IrOutboxBatchPlanStatus { Planned, Invalid, Unsupported };

struct IrOutboxBatchPlan {
  IrOutboxBatchPlanStatus status = IrOutboxBatchPlanStatus::Invalid;
  std::vector<std::int64_t> rowIds;
  std::string diagnostic;
};

virtual IrOutboxBatchPlan planBatch(
    std::span<const IrOutboxEntry> due) const;
virtual DeliveryOutcome submitBatch(
    std::span<const IrOutboxEntry> entries, bool userIntent,
    const IrProviderRuntimeConfig &, IrHttpClient &,
    std::stop_token) const;
virtual DeliveryOutcome pollBatch(
    std::span<const IrOutboxEntry> entries,
    const IrProviderRuntimeConfig &, IrHttpClient &,
    std::stop_token) const;

struct TachiOutboxBatchDocument {
  std::vector<std::int64_t> rowIds;
  std::string playtype;
  std::string payloadJson;
};

enum class BuildTachiOutboxBatchStatus { Built, Invalid };

struct BuildTachiOutboxBatchOutcome {
  BuildTachiOutboxBatchStatus status =
      BuildTachiOutboxBatchStatus::Invalid;
  std::optional<TachiOutboxBatchDocument> document;
  std::string diagnostic;
};

BuildTachiOutboxBatchOutcome buildBatchManualOutboxDocument(
    std::span<const IrOutboxEntry> entries) noexcept;
```

Add `std::vector<std::string> remoteScoreIds` and `bool importHadErrors` to `DeliveryOutcome`; retain `remoteScoreId` only for the exact one-ID compatibility case.

- [ ] **Step 1: Add failing generic driver fallback tests**

Assert the base driver plans the first row only, singular fallback accepts exactly one entry, and registry plan validation rejects duplicate, unknown, or out-of-input row IDs.

- [ ] **Step 2: Add failing Tachi composer tests**

Create valid 7K and 14K entries with distinct attempts. Assert:

```cpp
const auto batch = ir::tachi::buildBatchManualOutboxDocument(entries);
expect(batch.status == BuildTachiOutboxBatchStatus::Built,
       "compatible rows build a batch");
const auto json = nlohmann::json::parse(batch.document->payloadJson);
expect(json.at("meta").at("playtype") == "7K", "batch keeps playtype");
expect(json.at("scores").size() == entries.size(),
       "batch contains every compatible score exactly once");
```

Assert mixed playtypes select only the first compatible group, invalid proof/payload rejects before HTTP, 65 rows cap at 64, and gauge-heavy rows split at the last score keeping serialized payload at or below `kMaximumPayloadBytes`.

- [ ] **Step 3: Add failing response and HTTP tests**

Replace the old multiple-ID rejection test: multiple bounded IDs with empty errors succeed and populate `remoteScoreIds`; empty IDs/empty errors remains idempotent success. Multi-entry requests with non-empty errors become `ambiguous_partial_import`; one-entry one-ID warnings retain `accepted_with_warnings`. Assert `submitBatch` sends one POST/body and one user-intent header.

- [ ] **Step 4: Run focused tests to verify RED**

```bash
cmake --build cmake-build-debug --target ir_driver_tests tachi_batch_manual_tests tachi_driver_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(ir_driver_tests|tachi_batch_manual_tests|tachi_driver_tests)$'
```

Expected: missing batch contracts and the old parser restriction fail.

- [ ] **Step 5: Implement generic fallback and deterministic composition**

The default plan returns the first valid row. Default batch submission copies the only entry, applies `userIntent`, and calls the singular method. Registry wrappers catch exceptions and require 1–64 unique planned IDs present in `due`.

For Tachi, validate each proof, parse exactly one score/meta object from each payload, choose the first row's playtype, and append compatible due rows in order until 64 entries or the next serialized document exceeds 64 KiB. Skip later incompatible playtypes for the next worker iteration; reject an invalid first row. Awaiting plans group exact shared `remoteJobId` and normalized `remoteOrigin` and poll once.

- [ ] **Step 6: Implement batch-aware response classification**

Parse every bounded score ID and set `remoteScoreId` only when the vector size is one. Then apply:

```cpp
if (entries.size() > 1 && outcome.importHadErrors) {
  return permanent("ambiguous_partial_import",
                   "Tachi returned an ambiguous partial batch result.");
}
if (outcome.remoteScoreIds.size() > entries.size()) {
  return permanent("malformed_response",
                   "Tachi returned more score IDs than submitted scores.");
}
```

Fewer IDs with no errors is valid because equivalent scores may already exist.

- [ ] **Step 7: Run tests and commit**

```bash
cmake --build cmake-build-debug --target ir_driver_tests tachi_batch_manual_tests tachi_driver_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(ir_driver_tests|tachi_batch_manual_tests|tachi_driver_tests)$'
git diff --check
git add src/ir/IrDriver.* src/ir/tachi/TachiBatchManual.* \
  src/ir/tachi/TachiResponseParser.cpp src/ir/tachi/TachiDriver.* \
  tests/ir_driver_tests.cpp tests/tachi_batch_manual_tests.cpp \
  tests/tachi_driver_tests.cpp
git commit -m "feat: compose Tachi score batches"
```

Expected: provider tests pass and the compatible fixture records one HTTP request.

---

### Task 3: Claim, Poll, and Complete Outbox Groups Atomically

**Files:**
- Modify: `src/ir/IrOutboxModels.h`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryIrOutbox.cpp`
- Modify: `src/ir/IrSubmissionService.cpp`
- Modify: `tests/replay_repository_tests.cpp`
- Modify: `tests/ir_submission_service_tests.cpp`

**Interfaces:**
- Consumes: a validated `IrOutboxBatchPlan`.
- Produces:

```cpp
struct IrOutboxClaimRequest {
  std::int64_t rowId = 0;
  IrOutboxState expectedState = IrOutboxState::Pending;
};

struct IrOutboxBatchClaimOutcome {
  IrOutboxClaimStatus status = IrOutboxClaimStatus::StorageFailure;
  std::vector<IrOutboxEntry> entries;
  bool consumedUserIntent = false;
  std::string diagnostic;
};

IrOutboxBatchClaimOutcome ClaimIrOutboxBatch(
    std::span<const IrOutboxClaimRequest> requests,
    std::int64_t nowMs);

IrOutboxMutationOutcome ApplyIrOutboxDeliveries(
    std::span<const ir::IrOutboxDeliveryUpdate> updates);
```

Singular claim/apply methods delegate to one-item batches.

- [ ] **Step 1: Write failing atomic repository tests**

Claim three pending rows and assert one transaction advances all, increments attempts, consumes aggregate user intent, and returns input order. A state mismatch claims none. Apply shared deferred updates; then apply successful receipt updates. Inject receipt failure and assert the whole group remains Uploading.

- [ ] **Step 2: Upgrade the fake driver and write failing worker tests**

Add fake batch overrides and assert:

```cpp
expect(driver->waitForCalls(1), "one grouped request starts");
const auto calls = driver->calls();
expect(calls.size() == 1 && calls.front().rowIds.size() == 3,
       "three due attempts share one provider request");
```

Add mixed-plan two-call, shared deferred one-poll, cancellation, missing-credential, and ambiguous-partial tests.

- [ ] **Step 3: Run tests to verify RED**

```bash
cmake --build cmake-build-debug --target replay_repository_tests ir_submission_service_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(replay_repository_tests|ir_submission_service_tests)$'
```

Expected: missing group APIs and old one-row worker assumptions fail.

- [ ] **Step 4: Implement atomic claim and apply**

Validate non-empty input, 64-row maximum, unique positive IDs, and allowed states. Under one immediate transaction, load/validate all rows before any update, update/reload all, then commit. For apply, refactor singular delivery/receipt SQL into a connection helper and require exactly one affected row for each update before commit.

- [ ] **Step 5: Replace `runOne` transport with one planned group**

Plan after due sorting, resolve plan IDs, require a common request kind, check credentials, and claim atomically. Call `submitBatch` or `pollBatch` once. Build one delivery update/receipt per row; use a response score ID only for one input/one ID, otherwise store no guessed ID. Apply once, publish all statuses, refresh counts once, and call `submissionSucceeded` per committed chart.

- [ ] **Step 6: Run tests and commit**

```bash
cmake --build cmake-build-debug --target replay_repository_tests ir_submission_service_tests tachi_driver_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(replay_repository_tests|ir_submission_service_tests|tachi_driver_tests)$'
git diff --check
git add src/ir/IrOutboxModels.h src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryIrOutbox.cpp \
  src/ir/IrSubmissionService.cpp tests/replay_repository_tests.cpp \
  tests/ir_submission_service_tests.cpp
git commit -m "feat: deliver IR outbox rows in atomic batches"
```

Expected: grouped delivery/shared polling and single-row compatibility pass.

---

### Task 4: Build the Selectable Jacket-Rich Candidate List

**Files:**
- Create: `src/view/IrUploadCandidateListView.h`
- Create: `src/view/IrUploadCandidateListView.cpp`
- Create: `tests/ir_upload_candidate_list_view_tests.cpp`
- Modify: `src/view/ImageView.h`
- Modify: `src/view/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: candidates plus an external selected replay-ID set.
- Produces:

```cpp
class IrUploadCandidateListView
    : public RecyclerView<ir::IrUploadCandidate> {
public:
  void setCandidates(
      const std::vector<ir::IrUploadCandidate> &candidates,
      const std::unordered_set<int> &selectedReplayIds);
  void setSelectionLocked(bool locked);
  std::function<void(int replayId)> onSelectionToggle;
};
```

Add read-only `ImageView::imagePath()` returning `const path_t &` for recycled-jacket tests.

- [ ] **Step 1: Write failing headless view tests**

Use Noop bgfx. Bind/click an eligible row, then rebind to a failed row with another jacket and no selection:

```cpp
expect(text(row, "irUploadTitle")->getText() == "First Song",
       "row shows chart title");
expect(image(row, "irUploadJacket")->imagePath() == firstJacket,
       "row shows the first jacket");
list.setCandidates({second}, {});
expect(image(row, "irUploadJacket")->imagePath() == secondJacket,
       "rebind replaces jacket identity");
expect(text(row, "irUploadStatus")->getText() == "Retry",
       "rebind replaces status");
```

Also assert no-jacket cleanup, checkbox state, clear lamp/rank, combo/date/option text, lock suppression, and no event fallthrough.

- [ ] **Step 2: Register and run the test to verify RED**

Register the target with the new view, `ImageView.cpp`, basic view/rendering sources, archive/path/BMS sources, candidate sources and IR/provenance dependencies; link `${COMMON_LIBS} bgfx yogacore` plus platform libraries matching existing view targets.

```bash
cmake --build cmake-build-debug --target ir_upload_candidate_list_view_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^ir_upload_candidate_list_view_tests$'
```

Expected: compilation fails because the list view is absent.

- [ ] **Step 3: Implement complete bind replacement**

Use the Chart list's 84-pixel artwork frame and a 108-pixel row: checkbox, clear lamp, jacket, title/artist, attempt detail, difficulty/key-mode, score/rank, and status chip. Every bind replaces replay ID, selection, callbacks, text, colors, and image:

```cpp
const path_t jacket = candidate.chart.meta.StageFile.empty()
                          ? path_t{}
                          : candidate.chart.meta.Folder /
                                candidate.chart.meta.StageFile;
if (jacket.empty()) {
  jacketImage_->freeImage();
} else {
  jacketImage_->setImageAsync(jacket);
}
statusText_->setText(candidate.state == ir::IrRecordState::Failed
                         ? "Retry"
                         : "Eligible");
```

Capture replay ID by value in callbacks. Copy the selected-ID set before `setItems`.

- [ ] **Step 4: Run tests and commit**

```bash
cmake --build cmake-build-debug --target ir_upload_candidate_list_view_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(ir_upload_candidate_list_view_tests|result_record_list_view_tests|view_layout_tests)$'
git diff --check
git add src/view/IrUploadCandidateListView.* src/view/ImageView.h \
  src/view/CMakeLists.txt tests/ir_upload_candidate_list_view_tests.cpp \
  CMakeLists.txt
git commit -m "feat: render selectable IR upload rows"
```

Expected: selected view tests pass.


---

### Task 5: Read All-Chart Replay Candidates Explicitly

**Files:**
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryInternal.h`
- Modify: `src/repositories/ReplayRepositoryRecords.cpp`
- Modify: `tests/replay_repository_tests.cpp`

**Interfaces:**
- Consumes: provider ID, normalized server origin, replay provenance/outbox/receipt tables.
- Produces:

```cpp
enum class IrUploadReplayReadStatus {
  Loaded,
  Invalid,
  StorageFailure,
  IntegrityConflict,
};

struct IrUploadReplayReadOutcome {
  IrUploadReplayReadStatus status = IrUploadReplayReadStatus::StorageFailure;
  std::vector<ReplaySummary> replays;
  std::size_t omittedRows = 0;
  std::string diagnostic;
};

inline constexpr std::size_t kMaximumIrUploadCandidateRows = 16384;

IrUploadReplayReadOutcome ListIrUploadCandidateReplays(
    std::string_view providerId, std::string_view serverOrigin);
```

- [ ] **Step 1: Add failing repository coverage**

Persist two eligible attempts for one chart and one for another. Add receipt/outbox variants, a course stage, malformed provenance, and an oversized direct-row fixture. Assert newest-first all-chart results, stored partial chart identity, failed-row inclusion, queued/active/awaiting/blocked/succeeded exclusion, origin-scoped receipt suppression, invalid-origin failure, storage failure, and explicit oversized failure rather than a prefix.

```cpp
const auto loaded = helper.ListIrUploadCandidateReplays(
    "tachi", "https://boku.tachi.ac");
assert(loaded.status == IrUploadReplayReadStatus::Loaded);
assert((replayIds(loaded.replays) == std::vector<int>{newest, older, other}));
assert(loaded.replays[0].chartMeta->BmsPath == newestReplay.chartMeta.BmsPath);
```

- [ ] **Step 2: Run the repository test to verify RED**

```bash
cmake --build cmake-build-debug --target replay_repository_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^replay_repository_tests$'
```

Expected: compilation fails because the candidate read API does not exist.

- [ ] **Step 3: Implement one snapshot query with integrity checks**

Validate provider/origin, start one read transaction, select at most 16,385 rows, and use this boundary:

```sql
WHERE NOT EXISTS (
  SELECT 1 FROM course_replay_stages stage WHERE stage.replay_id = replay.id
)
AND replay.attempt_id IS NOT NULL
AND replay.attempt_fingerprint IS NOT NULL
AND NOT EXISTS (
  SELECT 1 FROM ir_submission_receipts receipt
  WHERE receipt.provider_id = ? AND receipt.server_origin = ?
    AND receipt.replay_id = replay.id
)
AND (outbox.id IS NULL OR outbox.state = 4)
ORDER BY replay.id DESC
LIMIT 16385
```

Join `ir_outbox` on provider/attempt. Read summary columns plus stored path, hashes, title, artist, and outbox state. Decode provenance; skip corrupt rows with one aggregate count. Require canonical UUID/fingerprint shapes and build partial `ChartMeta` without fabricating jacket, key-mode, or note-count data. Return no rows on overflow or snapshot failure.

- [ ] **Step 4: Run tests and commit**

```bash
cmake --build cmake-build-debug --target replay_repository_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^replay_repository_tests$'
git diff --check
git add src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryInternal.h \
  src/repositories/ReplayRepositoryRecords.cpp tests/replay_repository_tests.cpp
git commit -m "feat: list cross-chart IR upload candidates"
```

Expected: repository tests pass.

---

### Task 6: Bulk-Hydrate Candidate Chart Metadata

**Files:**
- Modify: `src/repositories/ChartRepository.h`
- Modify: `src/repositories/ChartRepositoryInternal.h`
- Modify: `src/repositories/ChartRepositoryQueries.cpp`
- Modify: `tests/chart_repository_tests.cpp`
- Modify: `tests/ir_upload_candidates_tests.cpp`

**Interfaces:**
- Consumes: up to 16,384 replay chart paths.
- Produces:

```cpp
enum class ChartMetaPathBatchReadStatus { Loaded, Invalid, StorageFailure };

struct ChartMetaPathBatchReadOutcome {
  ChartMetaPathBatchReadStatus status =
      ChartMetaPathBatchReadStatus::StorageFailure;
  std::vector<ChartMetaRecord> records;
  std::size_t missingPaths = 0;
  std::string diagnostic;
};

ChartMetaPathBatchReadOutcome SelectChartMetaByPaths(
    std::span<const std::filesystem::path> paths);
```

- [ ] **Step 1: Write failing bulk-read tests**

Insert charts with stage files, title/subtitle, artist, difficulty, key mode, hashes, and notes. Query two paths, a duplicate, and a missing path. Assert stable input-first order after deduplication, complete metadata, `missingPaths == 1`, and explicit empty/oversized/storage outcomes.

```cpp
const auto loaded = session->SelectChartMetaByPaths(
    {first.meta.BmsPath, second.meta.BmsPath, first.meta.BmsPath, missing});
assert(loaded.status == ChartMetaPathBatchReadStatus::Loaded);
assert(loaded.records.size() == 2);
assert(loaded.records[0].meta.StageFile == first.meta.StageFile);
assert(loaded.records[1].meta.TotalNotes == second.meta.TotalNotes);
assert(loaded.missingPaths == 1);
```

- [ ] **Step 2: Run the chart repository test to verify RED**

```bash
cmake --build cmake-build-debug --target chart_repository_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^chart_repository_tests$'
```

Expected: compilation fails because `SelectChartMetaByPaths` is missing.

- [ ] **Step 3: Implement chunked bulk lookup**

Normalize with `chart_storage_identity::StoredPathText`, reject more than 16,384 distinct non-empty paths, and preserve first occurrence order. Start one read transaction; build a parameterized `IN` list for each chunk of at most 256 paths using `kChartMetaSelectColumns`/`readChartMetaRecord`; reorder results by normalized input index. Return `StorageFailure` for prepare/bind/step/commit failures rather than empty success.

- [ ] **Step 4: Connect and test the full candidate load**

Extend the candidate test with replay paths, a bulk chart result, and projection. Assert missing charts are omitted and complete charts resolve identically to Records.

```bash
cmake --build cmake-build-debug --target chart_repository_tests ir_upload_candidates_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(chart_repository_tests|ir_upload_candidates_tests)$'
git diff --check
git add src/repositories/ChartRepository.h \
  src/repositories/ChartRepositoryInternal.h \
  src/repositories/ChartRepositoryQueries.cpp tests/chart_repository_tests.cpp \
  tests/ir_upload_candidates_tests.cpp
git commit -m "feat: hydrate IR upload chart metadata in batches"
```

Expected: both tests pass.

---

### Task 7: Queue or Retry Prepared Scores in One Durable Mutation

**Files:**
- Create: `src/ir/IrSavedResultBatchUpload.h`
- Create: `src/ir/IrSavedResultBatchUpload.cpp`
- Create: `tests/ir_saved_result_batch_upload_tests.cpp`
- Modify: `src/ir/IrOutboxModels.h`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryIrOutbox.cpp`
- Modify: `src/ir/IrSubmissionService.h`
- Modify: `src/ir/IrSubmissionService.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `tests/replay_repository_tests.cpp`
- Modify: `tests/ir_submission_service_tests.cpp`

**Interfaces:**
- Consumes: independently reconstructed `IrSubmission` values.
- Produces:

```cpp
enum class IrManualBatchItemStatus {
  Inserted,
  RetryQueued,
  AlreadyQueued,
  AlreadySubmitted,
  Failed,
};

struct IrManualBatchItemOutcome {
  std::string attemptId;
  IrManualBatchItemStatus status = IrManualBatchItemStatus::Failed;
  std::optional<IrOutboxEntry> entry;
  std::string diagnostic;
};

struct IrManualBatchEnqueueOutcome {
  bool storageAvailable = false;
  std::vector<IrManualBatchItemOutcome> items;
  std::string diagnostic;
};

IrManualBatchEnqueueOutcome EnqueueReadyIrOutboxDrafts(
    std::span<const ir::IrOutboxDraft> drafts,
    bool userIntent,
    std::int64_t nowMs);

IrManualBatchEnqueueOutcome IrSubmissionService::enqueueManualBatch(
    std::span<const IrOutboxDraft> drafts);

struct IrSavedResultBatchUploadDependencies {
  std::function<BuildDraftOutcome(const IrSubmission &)> buildDraft;
  std::function<IrManualBatchEnqueueOutcome(
      std::span<const IrOutboxDraft>)> enqueueBatch;
};

struct IrSavedResultBatchUploadResult {
  std::vector<IrManualBatchItemOutcome> items;
  std::size_t buildFailures = 0;
  std::string diagnostic;
};

IrSavedResultBatchUploadResult executeIrSavedResultBatchUpload(
    std::string_view providerId,
    std::span<const IrSubmission> submissions,
    const IrSavedResultBatchUploadDependencies &dependencies) noexcept;
```

- [ ] **Step 1: Write failing repository batch-mutation tests**

Seed absent, failed, pending, succeeded, and payload-conflicting attempts. Assert absent insert, failed retry, unchanged pending/succeeded, isolated logical conflict, one commit, and full rollback on injected SQLite failure.

- [ ] **Step 2: Write failing pure orchestration and service tests**

```cpp
int enqueueCalls = 0;
const auto result = ir::executeIrSavedResultBatchUpload(
    "tachi", submissions,
    {.buildDraft = buildFixtureDraft,
     .enqueueBatch = [&](std::span<const ir::IrOutboxDraft> drafts) {
       ++enqueueCalls;
       expect(drafts.size() == 2, "only valid drafts reach storage");
       return acceptedBatch(drafts);
     }});
expect(enqueueCalls == 1, "preparation performs one service mutation");
expect(result.buildFailures == 1, "invalid construction remains isolated");
```

Service tests assert provider availability is checked once, entries publish under one generation, counts refresh once, the worker wakes once, and singular `enqueueManual` delegates through one item.

- [ ] **Step 3: Register and run tests to verify RED**

Register `ir_saved_result_batch_upload_tests` with the new source and existing IR model dependencies.

```bash
cmake --build cmake-build-debug --target ir_saved_result_batch_upload_tests replay_repository_tests ir_submission_service_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(ir_saved_result_batch_upload_tests|replay_repository_tests|ir_submission_service_tests)$'
```

Expected: compilation fails on missing batch types/APIs.

- [ ] **Step 4: Implement the transaction and adapters**

Validate/deduplicate drafts before the transaction. Reuse prepared insert, identity lookup, and this retry update:

```sql
UPDATE ir_outbox
SET state=0, consecutive_failure_count=0, next_attempt_at_ms=NULL,
    next_request_user_intent=1, last_error_code=NULL,
    last_error_message=NULL, updated_at_ms=?
WHERE id=? AND state=4 AND local_result_ready=1
```

Require existing payload/hash/proof/time equality. Map `FailedPermanent` to `RetryQueued`; pending/uploading/awaiting/blocked to `AlreadyQueued`; succeeded to `AlreadySubmitted`. Roll back the complete transaction on SQLite failure while allowing per-item logical conflicts.

`enqueueManualBatch` accepts one enabled writable provider, calls the repository once with `safeNow`, publishes entries, refreshes once, and signals once. `executeIrSavedResultBatchUpload` builds every submission, records sanitized failures, then calls the batch enqueue dependency once.

- [ ] **Step 5: Run tests and commit**

```bash
cmake --build cmake-build-debug --target ir_saved_result_batch_upload_tests replay_repository_tests ir_submission_service_tests ir_saved_result_upload_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(ir_saved_result_batch_upload_tests|replay_repository_tests|ir_submission_service_tests|ir_saved_result_upload_tests)$'
git diff --check
git add src/ir/IrSavedResultBatchUpload.* src/ir/IrOutboxModels.h \
  src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryIrOutbox.cpp src/ir/IrSubmissionService.* \
  src/ir/CMakeLists.txt tests/ir_saved_result_batch_upload_tests.cpp \
  tests/replay_repository_tests.cpp tests/ir_submission_service_tests.cpp \
  CMakeLists.txt
git commit -m "feat: enqueue saved IR results atomically"
```

Expected: all selected tests pass.

---

### Task 8: Add the IR Uploads Scene, Progress Flow, and Settings Link

**Files:**
- Create: `src/scene/IrUploadsScene.h`
- Create: `src/scene/IrUploadsScene.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsScene.cpp`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Modify: `src/scene/CMakeLists.txt`
- Create: `scripts/check_ir_uploads_flow.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: candidate/chart reads, `ResultRecallBuilder`, draft building, and one `enqueueManualBatch` call.
- Produces:

```cpp
enum class SettingsDestination { Profile, Ir };

class SettingsScene : public Scene, public IRhythmControl {
public:
  explicit SettingsScene(
      ApplicationContext &context,
      SettingsDestination destination = SettingsDestination::Profile);
};

class IrUploadsScene final : public Scene {
public:
  explicit IrUploadsScene(ApplicationContext &context) : Scene(context) {}
  void init() override;
  void update(float dt) override;
  EventHandleResult handleEvents(SDL_Event &event) override;
  void renderScene() override;
  void cleanupScene() override;
};
```

- [ ] **Step 1: Write the failing integration audit**

Create/register `scripts/check_ir_uploads_flow.py` with exact contracts:

```python
require('make_unique<IrUploadsScene>(context)' in main_menu,
        'Song Select opens the dedicated scene')
require('enqueueManualBatch' in uploads_scene,
        'the page uses one service batch boundary')
require('SettingsDestination::Ir' in uploads_scene,
        'configuration targets the IR tab')
require('ListIrUploadCandidateReplays' in uploads_scene and
        'SelectChartMetaByPaths' in uploads_scene,
        'the page uses explicit batch reads')
reject('enqueueManual(' in uploads_scene,
       'the page must not queue selections one by one')
```

```bash
python3 scripts/check_ir_uploads_flow.py .
```

Expected: failure because scene/entry are absent.

- [ ] **Step 2: Add an explicit Settings destination**

Initialize the tab from the constructor:

```cpp
SettingsScene::SettingsScene(ApplicationContext &context,
                             SettingsDestination destination)
    : Scene(context),
      activeTab(destination == SettingsDestination::Ir
                    ? SettingsTab::Ir
                    : SettingsTab::Profile),
      lastLaidOutTab(activeTab) {}
```

Keep registered Settings default behavior. `Open IR Settings` uses a temporary IR-targeted Settings scene; its Back action returns to retained Main Menu.

- [ ] **Step 3: Build page shell and explicit load states**

Follow Music Player safe-area patterns. Build Back/title/count/Refresh, provider card, Select All/Clear/count, flexing list, distinct empty/error text, and sticky upload footer. `reloadCandidates()` performs one replay read, one chart batch read, projection, selection intersection, and scroll restore. Read only credential presence through `IrCredentialStore`.

- [ ] **Step 4: Implement selection and live refresh**

Row toggles mutate selected IDs and rebind. Select All inserts all candidate IDs; Clear empties them. Observe account-evidence and attempt-status revisions, coalescing changes into one reload per update tick.

- [ ] **Step 5: Implement cancellable local preparation**

Snapshot selection, lock controls, and launch one `std::jthread`. For each candidate:

```cpp
auto stored = context.replayRepository.LoadReplayResult(
    candidate.replayId(), candidate.chart.meta);
std::atomic_bool cancelled{stopToken.stop_requested()};
std::stop_callback stopCallback(stopToken,
                                [&cancelled] { cancelled = true; });
auto recalled = stored ? result_recall::BuildChartResult(
                            std::move(*stored), cancelled)
                       : result_recall::ChartBuildOutcome{};
if (!recalled.value || !recalled.value->historicalIr ||
    !recalled.value->historicalIr->submission) {
  recordFailure(candidate.replayId(),
                "This saved result could not be verified for IR.");
  continue;
}
submissions.push_back(*recalled.value->historicalIr->submission);
```

After verification, call the pure batch helper using `irDrivers.buildDraft` and exactly one `irSubmissionService->enqueueManualBatch`. Publish progress/result through a mutex mailbox. Refresh, remove queued items, retain failed selections, and show `N queued, M failed`. Back/cleanup requests stop and joins; already committed rows remain durable.

- [ ] **Step 6: Add the Song Select header entry**

Add button fields beside Music/Tasks. Click cancels preview and retains Main Menu:

```cpp
context.sceneManager->changeScene(
    std::make_unique<IrUploadsScene>(context), true);
```

Reset pointers during cleanup.

- [ ] **Step 7: Run audit/build/tests and commit**

```bash
python3 scripts/check_ir_uploads_flow.py .
cmake --build cmake-build-debug --target main ir_upload_candidate_list_view_tests ir_saved_result_batch_upload_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(ir_uploads_flow_audit|ir_upload_candidate_list_view_tests|ir_saved_result_batch_upload_tests|main_menu_settings_anchor_audit)$'
git diff --check
git add src/scene/IrUploadsScene.* src/scene/MainMenuScene.* \
  src/scene/SettingsScene.h src/scene/SettingsScene.cpp \
  src/scene/SettingsSceneLayout.cpp src/scene/CMakeLists.txt \
  scripts/check_ir_uploads_flow.py CMakeLists.txt
git commit -m "feat: add IR batch uploads page"
```

Expected: audit/tests pass and `main` links.

---

### Task 9: Regression Sweep and Final Verification

**Files:**
- Modify only a task-owned file when its focused test identifies a defect; rerun that task's red/green command before continuing.
- Update: `docs/superpowers/plans/2026-07-19-ir-batch-upload-page.md` checkbox state during execution.

**Interfaces:**
- Consumes: all feature tasks.
- Produces: clean built/tested source with no deployment.

- [ ] **Step 1: Run the focused feature suite**

```bash
cmake --build cmake-build-debug --target \
  ir_upload_candidates_tests ir_saved_result_batch_upload_tests \
  ir_upload_candidate_list_view_tests replay_repository_tests \
  chart_repository_tests ir_driver_tests tachi_batch_manual_tests \
  tachi_driver_tests ir_submission_service_tests \
  result_record_list_view_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R \
  '^(ir_upload_candidates_tests|ir_saved_result_batch_upload_tests|ir_upload_candidate_list_view_tests|replay_repository_tests|chart_repository_tests|ir_driver_tests|tachi_batch_manual_tests|tachi_driver_tests|ir_submission_service_tests|result_record_list_view_tests|ir_uploads_flow_audit|records_ir_marker_audit|main_menu_settings_anchor_audit)$'
```

Expected: all selected targets/tests pass.

- [ ] **Step 2: Run the complete configured suite**

```bash
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: zero failures.

- [ ] **Step 3: Run final source/build checks**

```bash
git diff --check
cmake --build cmake-build-debug --target main -j 6
git status --short
git log --oneline -10
```

Expected: no whitespace errors, desktop `main` succeeds, and status lists only intentional execution-plan checkbox changes if they remain uncommitted.

- [ ] **Step 4: Prepare the handoff summary**

Report the header entry/direct Settings link, included/excluded states, selection/local verification behavior, proof of one compatible multi-score POST and one shared deferred poll, focused/full CTest results, desktop build result, and confirmation that Firebase was not used.
