# IR Upload Failure Reasons Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show a sanitized reason on each IR Uploads attempt row when local verification, batch queueing, or later provider delivery fails.

**Architecture:** Durable provider-delivery diagnostics flow from the existing outbox row through `ReplaySummary` into `IrUploadCandidate`. Immediate verification and batch-queue diagnostics flow through `PreparationOutcome` into a controller-owned replay-ID map that survives candidate refreshes only for the current page session; the candidate row renders the merged newest reason.

**Tech Stack:** C++23, SDL2, Yoga/bgfx views, SQLite replay repository, existing IR outbox and batch-upload models, CMake/CTest.

## Global Constraints

- Show both immediate verification/batch-queue failures and later provider delivery failures.
- Immediate failure reasons last only for the current IR Uploads page session.
- Provider delivery reasons remain durable through the existing outbox; do not add a new table or migration.
- The newest session failure overrides an older durable reason for the same visible replay.
- Successful queueing clears the session reason; another failed retry replaces it.
- Cancellation is reported by the existing page summary and does not create new per-row failure reasons.
- Every displayed diagnostic is sanitized and bounded by `ir::kMaximumDiagnosticBytes`.
- API keys and credential material must not enter diagnostics, row models, logs, or persistence.
- Preserve provider-native batch queueing and delivery; do not issue one request per score.
- Do not deploy to Firebase while verifying this change.

---

## File Structure

- Modify `src/repositories/ReplayRepository.h`: carry the failed outbox diagnostic in `ReplaySummary`.
- Modify `src/repositories/ReplayRepositoryRecords.cpp`: select, validate, sanitize, and decode `ir_outbox.last_error_message` for IR upload candidates.
- Modify `src/ir/IrUploadCandidates.h/.cpp`: carry the durable failure reason in the row presentation model.
- Modify `src/scene/IrUploadsController.h/.cpp`: return per-replay preparation failures and retain session-only reasons across refreshes.
- Modify `src/scene/IrUploadsScene.cpp`: preserve specific replay reconstruction/proof diagnostics from the verifier.
- Modify `src/view/IrUploadCandidateListView.h/.cpp`: render and clear the new failure line on every bind.
- Modify `CMakeLists.txt`: link the diagnostic sanitizer into the focused controller test target.
- Extend `tests/replay_repository_tests.cpp`, `tests/ir_upload_candidates_tests.cpp`, `tests/ir_uploads_controller_tests.cpp`, and `tests/ir_upload_candidate_list_view_tests.cpp`.

---

### Task 1: Load and Project Durable Provider Failure Reasons

**Files:**
- Modify: `src/repositories/ReplayRepository.h:164-200`
- Modify: `src/repositories/ReplayRepositoryRecords.cpp:2395-2558`
- Modify: `src/ir/IrUploadCandidates.h:13-23`
- Modify: `src/ir/IrUploadCandidates.cpp:45-100`
- Test: `tests/replay_repository_tests.cpp:5904-6015`
- Test: `tests/ir_upload_candidates_tests.cpp:90-155`

**Interfaces:**
- Consumes: `ir_outbox.last_error_message` from the provider-scoped failed row already joined by `ListIrUploadCandidateReplays`.
- Produces:

```cpp
struct ReplaySummary {
  // Existing fields remain unchanged.
  std::optional<ir::IrOutboxState> requestedIrOutboxState;
  std::string requestedIrOutboxDiagnostic;
};

namespace ir {
struct IrUploadCandidate {
  ReplaySummary replay;
  ChartMetaRecord chart;
  IrRecordState state = IrRecordState::Hidden;
  std::string failureReason;

  [[nodiscard]] int replayId() const noexcept { return replay.id; }
};
}
```

- [ ] **Step 1: Write failing repository assertions for the durable diagnostic**

In `testListIrUploadCandidateReplaysUsesBoundedScopedSnapshot`, update the failed outbox fixture directly after staging so it contains an unsafe control byte and more than the supported diagnostic bound:

```cpp
const std::string storedFailure =
    std::string("provider rejected score") + static_cast<char>(0x01) +
    std::string(ir::kMaximumDiagnosticBytes, 'x');
execOrAbort(db.get(),
            "UPDATE ir_outbox SET last_error_message='" + storedFailure +
                "' WHERE attempt_id='" + other.attempt.attemptId + "'");
```

After loading, assert the failed replay has a sanitized bounded diagnostic while eligible rows do not:

```cpp
const ReplaySummary &failed = loaded.replays.back();
assert(failed.requestedIrOutboxState ==
       ir::IrOutboxState::FailedPermanent);
assert(!failed.requestedIrOutboxDiagnostic.empty());
assert(failed.requestedIrOutboxDiagnostic.size() <=
       ir::kMaximumDiagnosticBytes);
assert(failed.requestedIrOutboxDiagnostic.find(static_cast<char>(0x01)) ==
       std::string::npos);
assert(loaded.replays.front().requestedIrOutboxDiagnostic.empty());
```

Use SQL-safe fixture text or the existing SQLite binding helper rather than interpolating untrusted runtime input; the test string is fully controlled.

- [ ] **Step 2: Write a failing candidate projection assertion**

Set the failed `ReplaySummary` diagnostic in `testProjectsOnlyCanonicalActionableAttempts` and verify that only the failed candidate carries it:

```cpp
failed.requestedIrOutboxDiagnostic =
    std::string("invalid chart") + static_cast<char>(0x01) + " payload";

expect(projected.candidates[0].failureReason.empty() &&
           projected.candidates[1].failureReason ==
               "invalid chart  payload" &&
           projected.candidates[2].failureReason.empty(),
       "projection carries a sanitized durable reason only on its failed row");
```

- [ ] **Step 3: Run the focused tests and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target replay_repository_tests ir_upload_candidates_tests -j 6
./cmake-build-debug/replay_repository_tests
./cmake-build-debug/ir_upload_candidates_tests
```

Expected: compilation fails because `requestedIrOutboxDiagnostic` and `failureReason` do not exist.

- [ ] **Step 4: Extend the repository candidate read**

Add the replay field in `ReplayRepository.h`. Extend the upload-candidate SQL projection from `outbox.state` to both state and diagnostic:

```cpp
"replay.attempt_fingerprint,outbox.state,outbox.last_error_message "
```

Validate column 26 with the existing `nullableText` helper:

```cpp
!nullableInteger(25) || !nullableText(26)
```

Decode only a present text value and sanitize at the repository boundary:

```cpp
if (sqlite3_column_type(statement.get(), 26) == SQLITE_TEXT) {
  summary.requestedIrOutboxDiagnostic =
      ir::sanitizeDiagnostic(readText(statement.get(), 26));
}
```

Do not change the existing `AND (outbox.id IS NULL OR outbox.state = 4)` filter.

- [ ] **Step 5: Project the durable reason into the row model**

Add `failureReason` to `IrUploadCandidate` and assign the sanitized replay diagnostic when publishing a candidate:

```cpp
result.candidates.push_back({
    .replay = std::move(hydrated),
    .chart = chart,
    .state = state,
    .failureReason =
        state == IrRecordState::Failed
            ? sanitizeDiagnostic(replay.requestedIrOutboxDiagnostic)
            : std::string{},
});
```

Because `hydrated` is moved into the candidate, compute the reason before the move or read it from `hydrated` in the initializer without using a moved-from value.

- [ ] **Step 6: Run focused tests and commit**

Run:

```bash
cmake --build cmake-build-debug --target replay_repository_tests ir_upload_candidates_tests -j 6
./cmake-build-debug/replay_repository_tests
./cmake-build-debug/ir_upload_candidates_tests
git diff --check
```

Expected: both test executables exit 0 and the diff check is empty.

Commit:

```bash
git add src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryRecords.cpp \
  src/ir/IrUploadCandidates.h src/ir/IrUploadCandidates.cpp \
  tests/replay_repository_tests.cpp tests/ir_upload_candidates_tests.cpp
git commit -m "feat: load IR upload failure reasons"
```

---

### Task 2: Preserve Immediate Failure Reasons for the Page Session

**Files:**
- Modify: `src/scene/IrUploadsController.h:20-125`
- Modify: `src/scene/IrUploadsController.cpp:1-270`
- Modify: `tests/ir_uploads_controller_tests.cpp`
- Modify: `CMakeLists.txt:1683-1695`

**Interfaces:**
- Consumes: verification diagnostics and `IrSavedResultBatchUploadResult::items[*].diagnostic`.
- Produces:

```cpp
namespace ir_uploads {
struct PreparationFailureReason {
  int replayId = 0;
  std::string diagnostic;
};

struct PreparationOutcome {
  bool cancelled = false;
  std::vector<int> queuedReplayIds;
  std::vector<int> failedReplayIds;
  std::vector<PreparationFailureReason> failureReasons;
};
}
```

`Controller` gains:

```cpp
std::unordered_map<int, std::string> sessionFailureReasons_;
```

- [ ] **Step 1: Write failing preparation diagnostic tests**

Extend `testPreparationContinuesAfterFailureAndBatchesOnce` so replay 11's verifier diagnostic is retained:

```cpp
expect(outcome.failureReasons.size() == 1 &&
           outcome.failureReasons.front().replayId == 11 &&
           outcome.failureReasons.front().diagnostic ==
               "This saved result could not be verified for IR.",
       "preparation preserves the failed verifier reason by replay ID");
```

Extend `testThrowingVerifierDoesNotAbortOtherCandidates` with the safe fallback:

```cpp
expect(outcome.failureReasons.size() == 1 &&
           outcome.failureReasons.front().replayId == 31 &&
           outcome.failureReasons.front().diagnostic ==
               "This saved result could not be verified for IR.",
       "throw isolation publishes a safe verification fallback");
```

Give replay 41 a batch diagnostic in `testBatchOutcomesMapByAttemptId` and assert reordered mapping remains exact:

```cpp
{.attemptId = attemptId(41),
 .status = ir::IrManualBatchItemStatus::Failed,
 .diagnostic = "provider rejected this score"},

expect(outcome.failureReasons.size() == 1 &&
           outcome.failureReasons.front().replayId == 41 &&
           outcome.failureReasons.front().diagnostic ==
               "provider rejected this score",
       "reordered batch diagnostics map through attempt identity");
```

For compacted and duplicate outcomes, assert explicit fallback text rather than an empty reason:

```cpp
expect(failureReason(outcome, 50) == "IR batch enqueue returned no outcome." &&
           failureReason(outcome, 51) ==
               "IR batch enqueue returned no outcome.",
       "missing compacted outcomes receive a fail-closed reason");
expect(failureReason(outcome, 60) ==
           "IR batch enqueue returned ambiguous outcomes.",
       "duplicate outcomes receive an ambiguity reason");
```

Add a local `failureReason(const PreparationOutcome &, int)` test helper that scans the bounded vector and returns an empty view when absent.

- [ ] **Step 2: Write failing controller session-lifecycle tests**

Add one test that starts with a durable reason, applies a newer failed completion, refreshes candidates, and then completes a successful retry:

```cpp
auto failed = candidate(70);
failed.failureReason = "older server failure";
controller.replaceCandidates({failed});
controller.toggle(70);
(void)controller.beginPreparation();
controller.completePreparation({
    .failedReplayIds = {70},
    .failureReasons = {{.replayId = 70,
                        .diagnostic = "new verification failure"}},
});
expect(controller.candidates().front().failureReason ==
           "new verification failure",
       "latest session reason overrides durable state");

controller.replaceCandidates({failed});
expect(controller.candidates().front().failureReason ==
           "new verification failure",
       "candidate refresh preserves the page-session reason");

(void)controller.beginPreparation();
controller.completePreparation({.queuedReplayIds = {70}});
controller.replaceCandidates({failed});
expect(controller.candidates().front().failureReason ==
           "older server failure",
       "successful queueing clears only the session override");
```

Add a cancellation case that starts with a visible session reason, completes a cancelled outcome, refreshes, and confirms the reason is unchanged.

- [ ] **Step 3: Run the controller test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target ir_uploads_controller_tests -j 6
./cmake-build-debug/ir_uploads_controller_tests
```

Expected: compilation fails because `PreparationFailureReason`, `failureReasons`, and `IrUploadCandidate::failureReason` session merging are unavailable.

- [ ] **Step 4: Add bounded per-replay preparation reasons**

Add a private sanitizing helper in `IrUploadsController.cpp`:

```cpp
std::string failureReason(std::string_view diagnostic,
                          std::string_view fallback) {
  std::string result = ir::sanitizeDiagnostic(diagnostic);
  return result.empty() ? std::string(fallback) : result;
}
```

During verification, append one reason only when no submission was produced:

```cpp
if (verified.submission.has_value()) {
  submissionReplayIds.push_back(candidate.replayId());
  submissions.push_back(std::move(*verified.submission));
} else {
  outcome.failureReasons.push_back({
      .replayId = candidate.replayId(),
      .diagnostic = failureReason(
          verified.diagnostic,
          "This saved result could not be verified for IR."),
  });
}
```

Catch verifier exceptions without exposing `what()` and append the same safe fallback. For verified submissions that cannot cross the batch boundary, append these exact fallbacks:

```cpp
"IR batch enqueue is unavailable."
"IR batch enqueue returned no outcome."
"IR batch enqueue returned ambiguous outcomes."
"IR batch enqueue rejected this score."
```

For a unique failed batch item, prefer its sanitized per-item diagnostic; if it is empty, prefer the sanitized batch diagnostic; then use `IR batch enqueue rejected this score.`. For duplicate submission attempt IDs before enqueue, give every affected replay `Saved results have duplicate IR attempt identity.`.

Keep `failedReplayIds` and `queuedReplayIds` semantics unchanged. Do not create failure reasons for a cancelled outcome.

- [ ] **Step 5: Merge session reasons in the controller**

Include `<unordered_map>` in the header. In `completePreparation`, leave the session map unchanged for cancellation. Otherwise erase queued IDs and replace reasons for still-failed IDs:

```cpp
for (const int replayId : outcome.queuedReplayIds) {
  sessionFailureReasons_.erase(replayId);
}
for (const auto &failure : outcome.failureReasons) {
  if (failure.replayId > 0 &&
      std::ranges::find(outcome.failedReplayIds, failure.replayId) !=
          outcome.failedReplayIds.end()) {
    sessionFailureReasons_[failure.replayId] =
        failureReason(failure.diagnostic,
                      "IR upload preparation failed.");
  }
}
```

In `replaceCandidates`, preserve the durable `candidate.failureReason` unless a session override exists. Remove overrides whose replay IDs are no longer published:

```cpp
std::unordered_set<int> publishedReplayIds;
publishedReplayIds.reserve(candidates_.size());
for (auto &candidate : candidates_) {
  publishedReplayIds.insert(candidate.replayId());
  if (const auto found = sessionFailureReasons_.find(candidate.replayId());
      found != sessionFailureReasons_.end()) {
    candidate.failureReason = found->second;
  }
}
std::erase_if(sessionFailureReasons_, [&](const auto &entry) {
  return !publishedReplayIds.contains(entry.first);
});
```

Apply the same override to the already-published `candidates_` at completion so the controller state is immediately correct before the next repository refresh.

- [ ] **Step 6: Link the sanitizer into the focused target**

Add these existing sources to `ir_uploads_controller_tests` in `CMakeLists.txt`:

```cmake
src/ir/IrOutboxModels.cpp
src/Uuid.cpp
```

No new production source registration is required.

- [ ] **Step 7: Run focused tests and commit**

Run:

```bash
cmake --build cmake-build-debug --target ir_uploads_controller_tests -j 6
./cmake-build-debug/ir_uploads_controller_tests
git diff --check
```

Expected: the controller executable exits 0 and the diff check is empty.

Commit:

```bash
git add src/scene/IrUploadsController.h src/scene/IrUploadsController.cpp \
  tests/ir_uploads_controller_tests.cpp CMakeLists.txt
git commit -m "feat: retain IR upload preparation failures"
```

---

### Task 3: Render Failure Reasons on Attempt Rows

**Files:**
- Modify: `src/scene/IrUploadsScene.cpp:560-600`
- Modify: `src/view/IrUploadCandidateListView.h:25-50`
- Modify: `src/view/IrUploadCandidateListView.cpp:45-260`
- Test: `tests/ir_upload_candidate_list_view_tests.cpp`

**Interfaces:**
- Consumes: `IrUploadCandidate::failureReason` after durable/session merging.
- Produces: a named `TextView` called `irUploadFailure` whose text is empty or `Failed: <reason>`.

- [ ] **Step 1: Write the failing recycled-row view test**

Give the failed candidate a reason before binding:

```cpp
second.failureReason = "provider rejected this score";
```

Assert the row displays it:

```cpp
expect(text(row, "irUploadFailure") != nullptr &&
           text(row, "irUploadFailure")->getText() ==
               "Failed: provider rejected this score",
       "failed attempt row shows its upload reason");
```

Before rebinding an eligible candidate into the same recycled row, clear its reason and assert the old text is gone:

```cpp
third.failureReason.clear();
list.setCandidates({third}, {});
row = list.getViewByIndex(0);
expect(text(row, "irUploadFailure")->getText().empty(),
       "recycled eligible row clears another attempt's failure reason");
```

- [ ] **Step 2: Run the view test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target ir_upload_candidate_list_view_tests -j 6
./cmake-build-debug/ir_upload_candidate_list_view_tests
```

Expected: the test fails because `irUploadFailure` does not exist.

- [ ] **Step 3: Add the failure line to the row**

Add `TextView *failureText_ = nullptr;` after `attemptText_` in the item view. Construct and configure it with the existing UI font:

```cpp
failureText_ = new TextView(kUiFont, 13);
failureText_->setName("irUploadFailure");
failureText_->setHeight(18);
failureText_->setOverflow(TextView::TextOverflow::Hidden);
failureText_->setThemedColor(ui_theme::coral);
textColumn_->addView(failureText_);
```

The text column remains within the current fixed row: title 27, artist 19, attempt 18, failure 18, and three 2-pixel gaps total 88 pixels inside the 92-pixel padded content height.

Set the text on every bind, including the empty case:

```cpp
failureText_->setText(candidate.failureReason.empty()
                          ? std::string{}
                          : "Failed: " + candidate.failureReason);
```

Do not change the existing `Eligible`/`Retry` badge behavior.

- [ ] **Step 4: Preserve the specific scene verifier reason**

Update the verifier in `IrUploadsScene::startUpload` so replay load and reconstruction failures retain safe specific text:

```cpp
if (!stored) {
  return ir_uploads::VerificationOutcome{
      .diagnostic = "Saved replay data could not be loaded."};
}
auto recalled =
    result_recall::BuildChartResult(std::move(*stored), cancelled);
if (!recalled.value) {
  return ir_uploads::VerificationOutcome{
      .diagnostic = recalled.diagnostic.empty()
                        ? "This saved result could not be reconstructed."
                        : ir::sanitizeDiagnostic(recalled.diagnostic)};
}
if (!recalled.value->historicalIr ||
    !recalled.value->historicalIr->submission) {
  return ir_uploads::VerificationOutcome{
      .diagnostic = "This saved result has no verifiable IR proof."};
}
return ir_uploads::VerificationOutcome{
    .submission = *recalled.value->historicalIr->submission};
```

The controller remains responsible for final sanitization and fallback text.

- [ ] **Step 5: Run focused UI/controller tests and commit**

Run:

```bash
cmake --build cmake-build-debug --target \
  ir_upload_candidate_list_view_tests ir_uploads_controller_tests -j 6
./cmake-build-debug/ir_upload_candidate_list_view_tests
./cmake-build-debug/ir_uploads_controller_tests
git diff --check
```

Expected: both executables exit 0 and the diff check is empty.

Commit:

```bash
git add src/scene/IrUploadsScene.cpp \
  src/view/IrUploadCandidateListView.h \
  src/view/IrUploadCandidateListView.cpp \
  tests/ir_upload_candidate_list_view_tests.cpp
git commit -m "feat: show IR upload failure reasons"
```

---

### Task 4: Verify the Integrated Feature

**Files:**
- Verify only; modify earlier task files if a regression is found.

**Interfaces:**
- Consumes: the complete durable/session diagnostic pipeline.
- Produces: a clean local branch with no deployment or push.

- [ ] **Step 1: Format changed C++ lines and check the diff**

Run:

```bash
git clang-format --force --quiet HEAD~3 -- \
  src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryRecords.cpp \
  src/ir/IrUploadCandidates.h src/ir/IrUploadCandidates.cpp \
  src/scene/IrUploadsController.h src/scene/IrUploadsController.cpp \
  src/scene/IrUploadsScene.cpp \
  src/view/IrUploadCandidateListView.h \
  src/view/IrUploadCandidateListView.cpp \
  tests/replay_repository_tests.cpp \
  tests/ir_upload_candidates_tests.cpp \
  tests/ir_uploads_controller_tests.cpp \
  tests/ir_upload_candidate_list_view_tests.cpp
git diff --check
git status --short
git diff HEAD~3 -- src/scene/IrUploadsScene.cpp \
  src/scene/IrUploadsController.cpp \
  src/view/IrUploadCandidateListView.cpp \
  src/ir/IrUploadCandidates.cpp \
  src/repositories/ReplayRepositoryRecords.cpp
```

Expected: no whitespace errors; only the approved failure-reason pipeline is present. If formatting changes tracked files, amend the corresponding local commit or make one focused formatting commit before continuing.

- [ ] **Step 2: Run every focused target**

Run:

```bash
cmake --build cmake-build-debug --target \
  replay_repository_tests \
  ir_upload_candidates_tests \
  ir_uploads_controller_tests \
  ir_upload_candidate_list_view_tests -j 6
./cmake-build-debug/replay_repository_tests
./cmake-build-debug/ir_upload_candidates_tests
./cmake-build-debug/ir_uploads_controller_tests
./cmake-build-debug/ir_upload_candidate_list_view_tests
```

Expected: all four executables exit 0.

- [ ] **Step 3: Build the desktop app**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: `main` links successfully.

- [ ] **Step 4: Run the complete configured test suite**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

Expected: 100% of configured tests pass.

- [ ] **Step 5: Confirm local-only delivery**

Run:

```bash
git status --short
git log -5 --oneline
```

Expected: the working tree is clean and the feature exists only in local commits. Do not push, reply to GitHub threads, resolve reviews, or run either Firebase deployment script.
