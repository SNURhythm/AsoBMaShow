# Durable IR Receipts and Record State Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist a permanent Bokutachi receipt in the same transaction as outbox success and make Records badges show every live submission state with FontAwesome icons.

**Architecture:** Extend Tachi delivery outcomes with bounded remote identifiers, add a receipt table to the profile replay database, and make `ApplyIrOutboxDelivery` commit success and its receipt atomically. Replace the Records boolean marker with one pure semantic state resolver; `MainMenuScene` observes submission-service revisions and rebinds affected virtualized rows.

**Tech Stack:** C++23, nlohmann/json, SQLite, SDL2/Yoga views, FontAwesome Solid, CMake/CTest.

## Global Constraints

- The outbox remains the only durable source of unfinished network work.
- API keys remain runtime-only and must never enter outbox rows, receipt rows, payload diagnostics, logs, or view models.
- A successful outbox transition and receipt upsert are one SQLite transaction.
- Tachi's empty `scoreIDs` plus empty `errors` response is an idempotent success.
- Receipt state is scoped by provider and normalized server origin.
- Existing eligible, queued, active, waiting, blocked, failed, and uploaded states remain separately observable.
- Recycled list items replace icon, color, enabled state, callback, and identity on every bind.
- Follow red-green TDD for each behavior and commit after each independently passing task.
- Do not run Firebase deployment scripts; verification is local only.

---

## File Map

- Create `src/ir/IrReceiptModels.h` and `src/ir/IrReceiptModels.cpp`: bounded receipt types, validation, and semantic Records-state resolution.
- Modify `src/ir/IrDriver.h`: add optional remote user/score identifiers to `DeliveryOutcome`.
- Modify `src/ir/tachi/TachiResponseParser.cpp`: preserve import identity and accept idempotent duplicates.
- Modify `src/ir/IrOutboxModels.h` and `.cpp`: carry the successful receipt payload into the repository mutation.
- Modify `src/repositories/ReplayRepositorySchema.cpp`: migrate the replay database from version 8 to 9 with the receipt table.
- Modify `src/repositories/ReplayRepositoryIrOutbox.cpp`: atomically update outbox and receipt.
- Modify `src/repositories/ReplayRepositoryRecords.cpp`: expose origin-scoped receipt data in replay summaries.
- Modify `src/repositories/ReplayRepository.h`: publish receipt repository APIs and semantic summary fields.
- Modify `src/ir/IrSubmissionService.cpp`: construct the successful receipt mutation and publish the final revision only after commit.
- Modify `src/ir/tachi/TachiBatchManual.h` and `.cpp`: separate saved-result eligibility from outbox presentation.
- Modify `src/view/ReplaySummaryListView.h`: render `IR` plus FontAwesome state glyphs.
- Modify `src/scene/MainMenuScene.h` and `.cpp`: resolve states and observe live revisions while Records is open.
- Modify `src/ir/IrSettingsPresentation.h` and `.cpp`: add credential-invalidation ordering before key mutation.
- Modify `src/scene/SettingsSceneIr.cpp`: clear account-scoped receipt evidence before credential replacement/removal.
- Modify `src/ir/CMakeLists.txt` and root `CMakeLists.txt`: compile sources and register focused tests.
- Modify `tests/tachi_driver_tests.cpp`, `tests/replay_repository_tests.cpp`, `tests/ir_submission_service_tests.cpp`, `tests/tachi_batch_manual_tests.cpp`, and `tests/replay_summary_list_view_tests.cpp`.
- Create `tests/ir_receipt_models_tests.cpp`.

---

### Task 1: Preserve Tachi Import Identity and Idempotent Success

**Files:**
- Modify: `tests/tachi_driver_tests.cpp`
- Modify: `src/ir/IrDriver.h`
- Modify: `src/ir/tachi/TachiResponseParser.cpp`

**Interfaces:**
- Produces these additions to `ir::DeliveryOutcome`:

```cpp
std::optional<std::int64_t> remoteUserId;
std::optional<std::string> remoteScoreId;
```

- [ ] **Step 1: Add failing parser regressions**

Add tests beside the existing immediate and deferred response tests:

```cpp
void testImmediateImportPreservesIdentity() {
  const auto outcome = ir::tachi::parseImmediateImportResponse(
      R"({"success":true,"body":{"userID":42,"scoreIDs":["Tscore"],"errors":[]}})");
  expect(outcome.status == ir::DeliveryStatus::Succeeded,
         "identity import succeeds");
  expect(outcome.remoteUserId == 42, "user identity is retained");
  expect(outcome.remoteScoreId == "Tscore", "score identity is retained");
}

void testDuplicateImportIsIdempotentSuccess() {
  const auto outcome = ir::tachi::parseImmediateImportResponse(
      R"({"success":true,"body":{"userID":42,"scoreIDs":[],"errors":[]}})");
  expect(outcome.status == ir::DeliveryStatus::Succeeded,
         "duplicate import is successful");
  expect(outcome.code == "already_exists", "duplicate has stable code");
  expect(!outcome.remoteScoreId, "duplicate has no fabricated score ID");
}
```

Also prove that an empty score list with a non-empty `errors` array remains `PermanentFailure`, an invalid/negative `userID` is ignored rather than persisted, and an oversized score ID remains malformed.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target tachi_driver_tests -j 6
./cmake-build-debug/tachi_driver_tests
```

Expected: compile failure because `DeliveryOutcome` has no remote identity fields, or the new assertions fail.

- [ ] **Step 3: Implement bounded identity parsing**

Add the fields to `DeliveryOutcome`. In `parseImportDocument`, accept a positive signed/unsigned `userID` that fits `std::int64_t`, retain exactly one validated score ID, and use this branch order:

```cpp
if (scoreIds->size() == 1) {
  return {.status = DeliveryStatus::Succeeded,
          .remoteUserId = parsedUserId,
          .remoteScoreId = scoreIds->front().get<std::string>(),
          .code = diagnostic->empty() ? std::string{}
                                     : "accepted_with_warnings",
          .diagnostic = *diagnostic};
}
if (scoreIds->empty() && errors->empty()) {
  return {.status = DeliveryStatus::Succeeded,
          .remoteUserId = parsedUserId,
          .code = "already_exists"};
}
if (scoreIds->empty()) {
  return rejected(diagnostic->empty() ? "Tachi rejected the score"
                                      : *diagnostic);
}
```

Do not accept multiple score IDs for the single-score outbox contract.

- [ ] **Step 4: Run the focused test and verify GREEN**

```bash
cmake --build cmake-build-debug --target tachi_driver_tests -j 6
./cmake-build-debug/tachi_driver_tests
git diff --check
```

Expected: exit 0.

- [ ] **Step 5: Commit**

```bash
git add src/ir/IrDriver.h src/ir/tachi/TachiResponseParser.cpp tests/tachi_driver_tests.cpp
git commit -m "fix: retain Tachi import score identity"
```

---

### Task 2: Add Receipt Models and Replay Schema Version 9

**Files:**
- Create: `src/ir/IrReceiptModels.h`
- Create: `src/ir/IrReceiptModels.cpp`
- Modify: `src/ir/IrOutboxModels.h`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositorySchema.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `tests/ir_receipt_models_tests.cpp`
- Modify: `tests/replay_repository_tests.cpp`

**Interfaces:**
- Produces:

```cpp
namespace ir {
enum class IrReceiptConfirmationSource : int { Submission = 0, Snapshot = 1 };

struct IrSuccessfulReceiptDraft {
  std::string serverOrigin;
  std::optional<std::int64_t> remoteUserId;
  std::string remoteChartId;
  std::string remoteScoreId;
  IrReceiptConfirmationSource source = IrReceiptConfirmationSource::Submission;
  bool observedInSnapshot = false;
  std::int64_t confirmedAtUnixMillis = 0;
};

struct IrSubmissionReceipt {
  std::int64_t id = 0;
  std::string providerId;
  std::string serverOrigin;
  int replayId = 0;
  std::string attemptId;
  std::string chartMd5;
  std::string chartSha256;
  std::optional<std::int64_t> remoteUserId;
  std::string remoteChartId;
  std::string remoteScoreId;
  IrReceiptConfirmationSource source = IrReceiptConfirmationSource::Submission;
  bool observedInSnapshot = false;
  std::int64_t confirmedAtUnixMillis = 0;
};

enum class IrReceiptReadStatus { Found, NotFound, Invalid, StorageFailure };
struct IrReceiptReadOutcome {
  IrReceiptReadStatus status = IrReceiptReadStatus::StorageFailure;
  std::optional<IrSubmissionReceipt> receipt;
  std::string diagnostic;
};
}
```

- Extends `IrOutboxDeliveryUpdate` with:

```cpp
std::optional<IrSuccessfulReceiptDraft> successfulReceipt;
```

- Adds repository methods:

```cpp
ir::IrReceiptReadOutcome LoadIrSubmissionReceipt(
    std::string_view providerId, std::string_view serverOrigin,
    std::string_view attemptId);
ir::IrOutboxMutationOutcome ClearIrSubmissionReceipts(
    std::string_view providerId, std::string_view serverOrigin);
```

- [ ] **Step 1: Write failing model validation tests**

Create `tests/ir_receipt_models_tests.cpp` and assert that normalized HTTPS and explicitly allowed HTTP origins, canonical attempt IDs, valid hashes, positive optional user IDs, bounded remote IDs, known source values, and positive timestamps pass. Assert that credentials embedded in no field are possible because no credential member exists. Invalid origins, hashes, UUIDs, control characters, oversized IDs, and non-positive timestamps fail with bounded diagnostics.

- [ ] **Step 2: Write failing schema and migration tests**

In `tests/replay_repository_tests.cpp`, add tests that a fresh database and a version-8 fixture migrate to version 9 with this exact logical table:

```sql
CREATE TABLE ir_submission_receipts (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  provider_id TEXT NOT NULL,
  server_origin TEXT NOT NULL,
  replay_id INTEGER NOT NULL,
  attempt_id TEXT NOT NULL,
  chart_md5 TEXT,
  chart_sha256 TEXT NOT NULL,
  remote_user_id INTEGER,
  remote_chart_id TEXT,
  remote_score_id TEXT,
  confirmation_source INTEGER NOT NULL,
  observed_in_snapshot INTEGER NOT NULL DEFAULT 0,
  confirmed_at_ms INTEGER NOT NULL,
  UNIQUE(provider_id, server_origin, replay_id),
  CHECK(observed_in_snapshot IN (0, 1)),
  FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE
)
```

Require an index on `(provider_id, server_origin, attempt_id)` and an index on `(provider_id, server_origin, remote_score_id)`. Add a malformed-current-schema test that fails closed instead of silently replacing a user table.

- [ ] **Step 3: Register tests and verify RED**

Add `ir_receipt_models_tests` to root `CMakeLists.txt` and the registered CTest target list. Run:

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target ir_receipt_models_tests replay_repository_tests -j 6
```

Expected: compilation fails because the receipt types and schema do not exist.

- [ ] **Step 4: Implement models and schema migration**

Implement validation using `normalizeServerOrigin`, `uuid::isCanonicalLowerV4`, `isLowerHexDigest`, and the existing IR byte limits. Increment `ReplayRepository::kCurrentSchemaVersion` to 9. Add exact schema inspection alongside the outbox inspections; version 9 is considered current only when result outbox, IR outbox, receipt table, and both receipt indexes are exact.

Add `IrReceiptModels.cpp` to `src/ir/CMakeLists.txt` and every focused repository/service target that needs it.

- [ ] **Step 5: Run focused tests and verify GREEN**

```bash
cmake --build cmake-build-debug --target ir_receipt_models_tests replay_repository_tests -j 6
./cmake-build-debug/ir_receipt_models_tests
./cmake-build-debug/replay_repository_tests
git diff --check
```

Expected: exit 0.

- [ ] **Step 6: Commit**

```bash
git add src/ir/IrReceiptModels.* src/ir/IrOutboxModels.* src/ir/CMakeLists.txt \
  src/repositories/ReplayRepository.h src/repositories/ReplayRepositorySchema.cpp \
  tests/ir_receipt_models_tests.cpp tests/replay_repository_tests.cpp CMakeLists.txt
git commit -m "feat: add durable IR receipt schema"
```

---

### Task 3: Commit Outbox Success and Receipt Atomically

**Files:**
- Modify: `src/repositories/ReplayRepositoryIrOutbox.cpp`
- Modify: `src/ir/IrSubmissionService.cpp`
- Modify: `tests/replay_repository_tests.cpp`
- Modify: `tests/ir_submission_service_tests.cpp`

**Interfaces:**
- Consumes: `IrOutboxDeliveryUpdate::successfulReceipt` from Task 2 and `DeliveryOutcome::remoteUserId/remoteScoreId` from Task 1.
- Produces: a durable receipt before a `Succeeded` attempt revision is published.

- [ ] **Step 1: Add repository atomicity tests**

Add cases that save a replay with a canonical attempt, enqueue and activate its outbox, then apply:

```cpp
const auto applied = helper.ApplyIrOutboxDelivery({
    .rowId = rowId,
    .nextState = ir::IrOutboxState::Succeeded,
    .consecutiveFailureCount = 0,
    .remotePollCount = 0,
    .lastErrorCode = {},
    .lastErrorMessage = {},
    .updatedAtUnixMillis = 2'000,
    .completedAtUnixMillis = 2'000,
    .successfulReceipt = ir::IrSuccessfulReceiptDraft{
        .serverOrigin = "https://boku.tachi.ac",
        .remoteUserId = 42,
        .remoteScoreId = "Tscore",
        .confirmedAtUnixMillis = 2'000,
    },
});
```

Assert both the succeeded outbox and receipt are visible after commit. Add failure cases for missing receipt, missing replay attempt, invalid origin, and a trigger that aborts receipt insertion; every failure must leave the outbox in its claimed state and create no receipt. Assert replay deletion cascades the receipt.

- [ ] **Step 2: Add service propagation tests**

Extend the fake driver to return remote identity. Assert that after worker completion `repository.LoadIrSubmissionReceipt(...)` returns the IDs and `service.status(...).state` is `Succeeded`. Add the duplicate-success case with no remote score ID and prove it still creates a receipt.

- [ ] **Step 3: Run tests and verify RED**

```bash
cmake --build cmake-build-debug --target replay_repository_tests ir_submission_service_tests -j 6
./cmake-build-debug/replay_repository_tests
./cmake-build-debug/ir_submission_service_tests
```

Expected: receipt assertions fail and success is not atomic.

- [ ] **Step 4: Implement the repository transaction**

In `ApplyIrOutboxDelivery`, require `successfulReceipt` exactly when `nextState == Succeeded`. Start `BEGIN IMMEDIATE`, perform the existing compare-and-update, resolve `replays.id` by canonical `attempt_id`, then insert/upsert the receipt with provider and chart hashes copied from the claimed outbox row. Use:

```sql
ON CONFLICT(provider_id, server_origin, replay_id) DO UPDATE SET
  attempt_id = excluded.attempt_id,
  chart_md5 = excluded.chart_md5,
  chart_sha256 = excluded.chart_sha256,
  remote_user_id = COALESCE(excluded.remote_user_id, remote_user_id),
  remote_chart_id = CASE WHEN excluded.remote_chart_id <> ''
                         THEN excluded.remote_chart_id ELSE remote_chart_id END,
  remote_score_id = CASE WHEN excluded.remote_score_id <> ''
                         THEN excluded.remote_score_id ELSE remote_score_id END,
  confirmation_source = excluded.confirmation_source,
  observed_in_snapshot = MAX(observed_in_snapshot,
                             excluded.observed_in_snapshot),
  confirmed_at_ms = MAX(confirmed_at_ms, excluded.confirmed_at_ms)
```

Roll back on any failure. Never place the credential or payload JSON in the receipt statement.

- [ ] **Step 5: Populate the receipt in the service**

After the driver returns success and before calling the repository, normalize the actual request origin and set:

```cpp
update.successfulReceipt = IrSuccessfulReceiptDraft{
    .serverOrigin = *origin,
    .remoteUserId = outcome.remoteUserId,
    .remoteScoreId = outcome.remoteScoreId.value_or(std::string{}),
    .source = IrReceiptConfirmationSource::Submission,
    .observedInSnapshot = false,
    .confirmedAtUnixMillis = completedAt,
};
```

Keep callback invalidation and status refresh after `ApplyIrOutboxDelivery` reports `Updated`. A rolled-back mutation must not publish `Succeeded`.

- [ ] **Step 6: Run focused tests and verify GREEN**

```bash
cmake --build cmake-build-debug --target replay_repository_tests ir_submission_service_tests -j 6
./cmake-build-debug/replay_repository_tests
./cmake-build-debug/ir_submission_service_tests
git diff --check
```

Expected: exit 0.

- [ ] **Step 7: Commit**

```bash
git add src/repositories/ReplayRepositoryIrOutbox.cpp src/ir/IrSubmissionService.cpp \
  tests/replay_repository_tests.cpp tests/ir_submission_service_tests.cpp
git commit -m "feat: persist IR receipts with delivery success"
```

---

### Task 4: Resolve Semantic Records State from Receipt, Outbox, and Activity

**Files:**
- Modify: `src/ir/IrReceiptModels.h`
- Modify: `src/ir/IrReceiptModels.cpp`
- Modify: `src/ir/tachi/TachiBatchManual.h`
- Modify: `src/ir/tachi/TachiBatchManual.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryRecords.cpp`
- Modify: `tests/ir_receipt_models_tests.cpp`
- Modify: `tests/tachi_batch_manual_tests.cpp`
- Modify: `tests/replay_repository_tests.cpp`

**Interfaces:**
- Produces:

```cpp
enum class IrRecordState {
  Hidden,
  Eligible,
  Queued,
  Uploading,
  AwaitingRemote,
  Blocked,
  Failed,
  Uploaded,
};

enum class IrRecordActivity {
  None,
  Submitting,
  Polling,
};

struct IrRecordStateInput {
  bool eligible = false;
  bool hasReceipt = false;
  std::optional<IrOutboxState> outboxState;
  IrRecordActivity activity = IrRecordActivity::None;
};

[[nodiscard]] IrRecordState
resolveIrRecordState(IrRecordStateInput input) noexcept;
```

- Replaces `ReplaySummary::irUploadPending` with:

```cpp
bool irSubmissionEligible = false;
bool hasIrReceipt = false;
std::string receiptRemoteScoreId;
IrRecordState irRecordState = IrRecordState::Hidden;
```

- [ ] **Step 1: Write the state matrix test**

Cover every state and precedence. Receipt or succeeded outbox produces `Uploaded`; active POST produces `Uploading`; active poll or awaiting row produces `AwaitingRemote`; pending produces `Queued`; blocked/failed remain distinct; eligible with no work produces `Eligible`; ineligible with no evidence produces `Hidden`. Receipt takes precedence over stale failed/pending rows.

- [ ] **Step 2: Separate eligibility from marker visibility**

Add a failing test for:

```cpp
bool isReplayEligibleForBokutachi(
    std::string_view attemptId, bool hasCanonicalAttemptFingerprint,
    const bms_parser::ChartMeta &meta,
    const ScoreProvenance &provenance) noexcept;
```

It contains all current lightweight proof checks but no outbox state. Keep `shouldShowReplayUploadMarker` temporarily as a compatibility wrapper implemented from eligibility plus `outboxState != Succeeded`, then remove its UI use.

- [ ] **Step 3: Add repository summary tests**

Extend `ListReplays` with an optional fourth argument, `irServerOrigin`. Test that provider plus normalized origin exposes a matching receipt and remote score ID; another origin does not. Calls without provider/origin retain existing ghost/replay behavior.

- [ ] **Step 4: Run tests and verify RED**

```bash
cmake --build cmake-build-debug --target ir_receipt_models_tests tachi_batch_manual_tests replay_repository_tests -j 6
./cmake-build-debug/ir_receipt_models_tests
./cmake-build-debug/tachi_batch_manual_tests
./cmake-build-debug/replay_repository_tests
```

Expected: compile or assertion failure.

- [ ] **Step 5: Implement state resolver and origin-scoped summary query**

Add receipt existence and `remote_score_id` subqueries to the replay detail snapshot only when both provider and origin are supplied. Normalize the origin before binding. Keep all summary reads inside the existing read transaction.

In `MainMenuScene::showReplayListModal`, calculate eligibility once, read the attempt status snapshot, map the service-only `IrActiveRequestKind` into the provider-neutral `IrRecordActivity`, and call `resolveIrRecordState`; do not set a second boolean marker or make receipt models depend on `IrSubmissionService.h`.

- [ ] **Step 6: Run tests and verify GREEN**

```bash
cmake --build cmake-build-debug --target ir_receipt_models_tests tachi_batch_manual_tests replay_repository_tests -j 6
./cmake-build-debug/ir_receipt_models_tests
./cmake-build-debug/tachi_batch_manual_tests
./cmake-build-debug/replay_repository_tests
git diff --check
```

Expected: exit 0.

- [ ] **Step 7: Commit**

```bash
git add src/ir/IrReceiptModels.* src/ir/tachi/TachiBatchManual.* \
  src/repositories/ReplayRepository.h src/repositories/ReplayRepositoryRecords.cpp \
  tests/ir_receipt_models_tests.cpp tests/tachi_batch_manual_tests.cpp \
  tests/replay_repository_tests.cpp
git commit -m "refactor: resolve semantic IR record states"
```

---

### Task 5: Render FontAwesome State Badges and Refresh Live Records

**Files:**
- Modify: `src/view/ReplaySummaryListView.h`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `tests/replay_summary_list_view_tests.cpp`
- Modify: `scripts/check_records_ir_marker.py`

**Interfaces:**
- Consumes: `ReplaySummary::irRecordState` and `IrSubmissionService::status(providerId, attemptId)` after mapping its active request into `IrRecordActivity`.
- Produces: an in-place list refresh after every service revision.

- [ ] **Step 1: Add virtualized badge binding tests**

Bind one item successively to all states and assert visibility, clickability, normal-font text `IR`, FontAwesome font path, and codepoint:

```cpp
constexpr std::array expected{
    std::pair{ir::IrRecordState::Eligible, 0xf0ee},
    std::pair{ir::IrRecordState::Queued, 0xf017},
    std::pair{ir::IrRecordState::Uploading, 0xf2f1},
    std::pair{ir::IrRecordState::AwaitingRemote, 0xf252},
    std::pair{ir::IrRecordState::Blocked, 0xf084},
    std::pair{ir::IrRecordState::Failed, 0xf071},
    std::pair{ir::IrRecordState::Uploaded, 0xf00c},
};
```

Prove only `Eligible` and `Failed` emit an upload request; active and uploaded badges consume the pointer without selecting the row. Rebind from remote state to hidden and prove no icon/callback leaks.

- [ ] **Step 2: Run the view test and verify RED**

```bash
cmake --build cmake-build-debug --target replay_summary_list_view_tests -j 6
./cmake-build-debug/replay_summary_list_view_tests
```

Expected: the binary boolean badge cannot express the matrix.

- [ ] **Step 3: Implement the two-font badge**

Use a horizontal content view containing `TextView("assets/fonts/notosanscjkjp.ttf", 14)` for `IR` and `TextView("assets/fonts/fa-solid-900.ttf", 14)` for `ui_icons::textForCodepoint(codepoint)`. Centralize state-to-icon/color/action mapping in one helper. Use amber for eligible/queued, cyan for active/polling, yellow for blocked, coral for failed, and lime for uploaded.

Delete the local `irUploadBusy` presentation override; activity comes from the service snapshot. Preserve a visible disabled badge as an event sink for non-actionable states.

- [ ] **Step 4: Add a live-refresh source-contract test**

Update `scripts/check_records_ir_marker.py` to require:

- an observed revision map keyed by attempt ID;
- polling only while the Records root and list content are visible;
- `refreshReplayIrMarker` after a revision change;
- selection and scroll restoration;
- no API-key lookup in `MainMenuScene`.

Run it before implementation and expect failure.

- [ ] **Step 5: Observe service revisions in `MainMenuScene::update`**

Add:

```cpp
std::unordered_map<std::string, std::uint64_t> replayIrObservedRevisions;
```

While Records is visible, inspect each local summary with an attempt ID. When `status.revision` differs, update the map and refresh that row from the repository using the active normalized origin. Re-resolve the semantic state with `status.activeRequest`. Preserve scroll offset and selected record identity across filtering/rebinding. Clear observed revisions when the modal closes or profile changes.

Change upload guards to allow `Eligible` and `Failed`; `Failed` follows the existing retry path. Queued, active, blocked, and uploaded taps only publish bounded feedback.

- [ ] **Step 6: Run tests and verify GREEN**

```bash
cmake --build cmake-build-debug --target replay_summary_list_view_tests -j 6
./cmake-build-debug/replay_summary_list_view_tests
python3 scripts/check_records_ir_marker.py
git diff --check
```

Expected: exit 0.

- [ ] **Step 7: Commit**

```bash
git add src/view/ReplaySummaryListView.h src/scene/MainMenuScene.h \
  src/scene/MainMenuScene.cpp tests/replay_summary_list_view_tests.cpp \
  scripts/check_records_ir_marker.py
git commit -m "fix: refresh Records IR badge states live"
```

---

### Task 6: Isolate Receipts When the Credential Changes

**Files:**
- Modify: `src/scene/SettingsSceneIr.cpp`
- Modify: `tests/ir_settings_presentation_tests.cpp`
- Modify: `scripts/check_main_menu_settings_anchor.py`

**Interfaces:**
- Consumes: `ReplayRepository::ClearIrSubmissionReceipts(providerId, serverOrigin)`.
- Produces: no stale checkmark after API-key replacement/removal.

- [ ] **Step 1: Add credential dependency-order tests**

Extend the settings action dependencies with an `invalidateRemoteIdentity` callback. Test that replacement/removal reject before credential mutation when invalidation fails, and call `credentialCommitted` only after both invalidation and credential write succeed.

- [ ] **Step 2: Run the focused test and verify RED**

```bash
cmake --build cmake-build-debug --target ir_settings_presentation_tests -j 6
./cmake-build-debug/ir_settings_presentation_tests
```

Expected: no invalidation dependency exists.

- [ ] **Step 3: Wire receipt invalidation**

Before changing the key, normalize the current server origin and clear the origin-scoped receipts plus cached Bokutachi user IDs. Reject credential mutation if either durable invalidation fails. Do not clear local replays or local score rows. Keep the existing service/ranking invalidation callback after credential commit.

- [ ] **Step 4: Run focused and full verification**

```bash
cmake --build cmake-build-debug --target ir_settings_presentation_tests -j 6
./cmake-build-debug/ir_settings_presentation_tests
python3 scripts/check_main_menu_settings_anchor.py
ctest --test-dir cmake-build-debug --output-on-failure
cmake --build cmake-build-debug --target main -j 6
git diff --check
```

Expected: every test passes and `main` builds.

- [ ] **Step 5: Commit**

```bash
git add src/ir/IrSettingsPresentation.* src/scene/SettingsSceneIr.cpp \
  tests/ir_settings_presentation_tests.cpp scripts/check_main_menu_settings_anchor.py
git commit -m "fix: isolate IR receipts across credentials"
```

---

## Plan 1 Completion Gate

- Every successful new upload has a durable origin-scoped receipt before the UI reports success.
- Existing retained successful outbox rows still display uploaded during compatibility.
- Records badges update from queued through uploaded without closing the modal.
- No reconciliation requests or imported remote score rows exist yet; those are Plan 2.
- Run `git status --short --branch` and confirm only intentional committed changes remain.
