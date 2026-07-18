# Bokutachi IR Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add modular, profile-aware Bokutachi score submission and chart rankings with crash-safe SQLite delivery, Tachi 202 polling, manual controls, and a shared rankings modal, while keeping credentials out of the outbox and preparing the driver boundary for a future read-only LR2IR implementation.

**Architecture:** Canonical IR models and a capability-checked driver registry sit between gameplay/UI and provider code. Automatic Tachi payload drafts join the existing replay and pending-score transaction in `replays.db`; a profile-bound worker activates them only after local score projection and delivers or polls them with the current credential. Ranking reads use a separate cancellable, memory-cached service and a shared modal; no ranking data is persisted.

**Tech Stack:** C++23, SDL2/Yoga views, SQLite, nlohmann JSON, libcurl on desktop/Android, `NSURLSession` through the existing iOS native bridge, CMake/CTest, `std::jthread`/`std::stop_token`.

## Global Constraints

- Implement Bokutachi/Tachi only. Do not register an LR2IR driver or parse LR2IR archives in this change.
- Treat `readOnly` as an enforced driver invariant: read-only drivers may serve rankings but cannot build, enqueue, submit, or poll score rows.
- Never serialize or log an API key in `settings.json`, `replays.db`, Batch Manual JSON, diagnostics, caches, profile archives, duplicated profiles, or test failure output.
- Never issue an authenticated redirect. Persist a validated origin with every deferred import and poll that exact origin.
- Do not let a network worker claim an automatic row until the local replay and score are durable and the combined acknowledgement/activation transaction commits.
- Keep application-facing IR boundaries non-throwing and convert exceptions to bounded typed outcomes.
- Do not add normal C++ source files to the iOS synchronized-group exclusion list. Keep the iOS-specific implementation in the existing `iOSNatives.mm` bridge.
- Do not deploy a mobile build. Use build-only verification commands.

## File Map

| Area | Files |
|---|---|
| Canonical models and drivers | `src/ir/IrSubmission.*`, `src/ir/IrRankingModels.*`, `src/ir/IrOutboxModels.*`, `src/ir/IrDriver.*` |
| Profile configuration | `src/ir/IrProfileSettings.*`, `src/ir/IrCredentialStore.*`, `src/AppSettings.*`, `src/AppSettingsStore.*`, `src/PlayerProfile.*`, `src/PlayerProfileManager.cpp` |
| Durable queue | `src/repositories/ReplayRepository.h`, `src/repositories/ReplayRepositorySchema.cpp`, `src/repositories/ReplayRepositoryRecords.cpp`, new `src/repositories/ReplayRepositoryIrOutbox.cpp` |
| Tachi protocol | `src/ir/tachi/TachiBatchManual.*`, `src/ir/tachi/TachiResponseParser.*`, `src/ir/tachi/TachiRankingParser.*`, `src/ir/tachi/TachiDriver.*` |
| HTTP | `src/ir/IrHttpClient.*`, `src/ir/IrHttpClientIOS.h`, `src/iOSNatives.mm` |
| Background services | `src/ir/IrSubmissionService.*`, `src/ir/IrRankingService.*`, `src/context.h`, `src/ProfileSessionCoordinator.*`, `src/main.cpp` |
| Gameplay/result integration | `src/scene/play/GamePlayScene.cpp`, `src/scene/ResultScene.*`, new `src/ir/IrResultPresentation.*` |
| Settings UI | `src/scene/SettingsScene.*`, new `src/scene/SettingsSceneIr.cpp`, new `src/ir/IrSettingsPresentation.*` |
| Rankings UI | `src/ir/IrRankingModal.*`, `src/scene/MainMenuScene.*`, `src/scene/ResultScene.*` |
| Portable profile hygiene | `src/ProfileArchive.cpp`, `src/PlayerProfileManager.cpp` |
| Build/tests | `src/CMakeLists.txt`, new `src/ir/CMakeLists.txt`, `src/scene/CMakeLists.txt`, root `CMakeLists.txt`, focused tests under `tests/` |

---

### Task 1: Add canonical IR models and enforce driver capabilities

**Files:**
- Create: `src/ir/IrSubmission.h`
- Create: `src/ir/IrSubmission.cpp`
- Create: `src/ir/IrRankingModels.h`
- Create: `src/ir/IrRankingModels.cpp`
- Create: `src/ir/IrOutboxModels.h`
- Create: `src/ir/IrOutboxModels.cpp`
- Create: `src/ir/IrDriver.h`
- Create: `src/ir/IrDriver.cpp`
- Create: `src/ir/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `tests/ir_driver_tests.cpp`

- [ ] Write `tests/ir_driver_tests.cpp` first. Cover canonical submission/query validation, all valid capability combinations, rejection of no-operation/read-only-submission/deferred-without-submit combinations, duplicate provider IDs, and the base class's typed unsupported outcomes.

- [ ] Add an `ir_driver_tests` executable and CTest registration. Include only the canonical model and driver sources so this test does not acquire SQLite, SDL rendering, or networking dependencies.

- [ ] Run the focused test and confirm it fails because the IR types do not exist:

```bash
cmake --build cmake-build-debug --target ir_driver_tests -j 6
```

Expected: compilation fails on missing `src/ir/IrDriver.h` or equivalent symbols.

- [ ] Implement these canonical boundaries in namespace `ir`:

```cpp
struct IrSubmission {
  std::string attemptId;
  int keyMode = 0;
  std::string chartMd5;
  std::string chartSha256;
  int score = 0;
  int maxScore = 0;
  int maxCombo = 0;
  int comboBreak = 0;
  int pGreat = 0;
  int great = 0;
  int good = 0;
  int bad = 0;
  int poor = 0;
  int kPoor = 0;
  int fast = 0;
  int slow = 0;
  float finalGauge = 0.0F;
  int clearType = kClearTypeFailedRank;
  std::int64_t playedAtUnixMillis = 0;
  ScoreProvenance provenance = ScoreProvenance::Legacy();
};

struct IrChartQuery {
  int keyMode = 0;
  std::string chartMd5;
  std::string chartSha256;
  int totalNotes = 0;
};

struct IrDriverCapabilities {
  bool readOnly = false;
  bool chartRankings = false;
  bool scoreSubmission = false;
  bool deferredSubmission = false;
};
```

Add `makeIrSubmission(const result_persistence::ChartResultAttempt &, std::int64_t)` and `makeIrChartQuery(const bms_parser::ChartMeta &)`. Normalize hash case and reject non-canonical UUIDs, malformed present hashes, missing identity, invalid counts, inconsistent EX/max scores, non-finite gauge, invalid clear rank, nonpositive timestamps, nonpositive key mode, and nonpositive ranking note count. Keep provider support such as Tachi's 7K/14K restriction in the driver rather than the canonical builder.

- [ ] Define provider-neutral outcomes and queue types. Keep diagnostics bounded to 512 UTF-8 bytes when constructed:

```cpp
enum class BuildDraftStatus { Built, Unsupported, Invalid };
enum class DeliveryStatus {
  Succeeded,
  Deferred,
  Ongoing,
  TransientFailure,
  BlockedConfiguration,
  PermanentFailure,
  Unsupported,
  Cancelled,
};
enum class ChartRankingStatus {
  Succeeded,
  ChartNotFound,
  AuthenticationRequired,
  TransientFailure,
  Unsupported,
  MalformedResponse,
  OversizedResponse,
  Cancelled,
};

struct IrOutboxDraft {
  std::string providerId;
  std::string attemptId;
  std::string chartMd5;
  std::string chartSha256;
  std::string payloadJson;
  std::int64_t createdAtUnixMillis = 0;
};

struct DeliveryOutcome {
  DeliveryStatus status = DeliveryStatus::PermanentFailure;
  std::optional<std::string> remoteJobId;
  std::optional<std::string> remoteOrigin;
  std::optional<std::chrono::milliseconds> retryAfterDelay;
  std::string code;
  std::string diagnostic;
};

struct IrChartRankingEntry {
  int rank = 0;
  std::string playerName;
  int score = 0;
  int maxScore = 0;
  int clearType = kClearTypeFailedRank;
  std::optional<int> badPoints;
  std::optional<int> maxCombo;
  std::optional<std::int64_t> achievedAtUnixMillis;
  bool currentUser = false;
};

struct IrChartRanking {
  std::string providerId;
  IrChartQuery chart;
  std::vector<IrChartRankingEntry> entries;
  std::int64_t fetchedAtUnixMillis = 0;
};

struct BuildDraftOutcome {
  BuildDraftStatus status = BuildDraftStatus::Invalid;
  std::optional<IrOutboxDraft> draft;
  std::string diagnostic;
};

struct ChartRankingOutcome {
  ChartRankingStatus status = ChartRankingStatus::MalformedResponse;
  std::optional<IrChartRanking> ranking;
  std::string diagnostic;
};

struct IrProviderRuntimeConfig {
  std::string profileId;
  std::string serverOrigin;
  std::string apiKey; // in-memory only; never serialize this object
};
```

- [ ] Implement `IrDriver` defaults and `IrDriverRegistry`. Registration owns `std::shared_ptr<const IrDriver>`, rejects an empty/duplicate provider ID, and enforces:

```cpp
const bool valid =
    (capabilities.chartRankings || capabilities.scoreSubmission) &&
    (!capabilities.readOnly ||
     (!capabilities.scoreSubmission && !capabilities.deferredSubmission)) &&
    (!capabilities.deferredSubmission || capabilities.scoreSubmission);
```

Use this driver shape, with base definitions returning the corresponding typed `Unsupported` outcome:

```cpp
class IrHttpClient;
struct IrOutboxEntry;

class IrDriver {
public:
  virtual ~IrDriver() = default;
  virtual std::string_view providerId() const noexcept = 0;
  virtual IrDriverCapabilities capabilities() const noexcept = 0;
  virtual BuildDraftOutcome buildDraft(const IrSubmission &) const;
  virtual DeliveryOutcome submit(const IrOutboxEntry &,
                                 const IrProviderRuntimeConfig &,
                                 IrHttpClient &,
                                 std::stop_token) const;
  virtual DeliveryOutcome poll(const IrOutboxEntry &,
                               const IrProviderRuntimeConfig &,
                               IrHttpClient &,
                               std::stop_token) const;
  virtual ChartRankingOutcome fetchChartRanking(
      const IrChartQuery &,
      const IrProviderRuntimeConfig &,
      IrHttpClient &,
      std::stop_token) const;
};
```

Expose capability-gated registry dispatch helpers with the same operation names. The registry must return `Unsupported` without invoking a read-only fake driver's submission methods.

- [ ] Add `add_subdirectory(ir)` to `src/CMakeLists.txt`; list all supported sources in `src/ir/CMakeLists.txt`. Keep provider-neutral files free of Tachi includes.

- [ ] Run and pass the test:

```bash
cmake --build cmake-build-debug --target ir_driver_tests -j 6
ctest --test-dir cmake-build-debug -R '^ir_driver_tests$' --output-on-failure
```

Expected: one passing test and no credential-like data in test output.

- [ ] Commit:

```bash
git add src/ir src/CMakeLists.txt CMakeLists.txt tests/ir_driver_tests.cpp
git commit -m "feat: add modular IR driver boundary"
```

---

### Task 2: Build stable one-score Tachi Batch Manual payloads

**Files:**
- Create: `src/ir/tachi/TachiBatchManual.h`
- Create: `src/ir/tachi/TachiBatchManual.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `tests/tachi_batch_manual_tests.cpp`

- [ ] Write table-driven tests for 7K/14K, SHA-256 preference, MD5 fallback, all eight local clear ranks, judgement fields, KPoor omission, `bp = bad + poor`, gauge clamping, and the captured Unix-millisecond time.

- [ ] Add rejection cases for unsupported key modes, missing/invalid hashes, negative counts, `score != pGreat * 2 + great`, `maxScore <= 0`, out-of-range score/combo, invalid timestamp, non-finite gauge, integer overflow in BP, and payloads beyond the chosen 64 KiB draft cap.

- [ ] Add and run `tachi_batch_manual_tests`; confirm it fails before implementation:

```bash
cmake --build cmake-build-debug --target tachi_batch_manual_tests -j 6
```

- [ ] Implement a pure builder returning `BuildDraftOutcome`. Emit exactly this document shape, with no URL or credential fields:

```cpp
nlohmann::json document = {
    {"meta", {{"game", "bms"},
              {"playtype", submission.keyMode == 7 ? "7K" : "14K"},
              {"service", "AsoBMaShow"}}},
    {"scores", nlohmann::json::array({{
         {"score", submission.score},
         {"lamp", lamp},
         {"matchType", "bmsChartHash"},
         {"identifier", identifier},
         {"timeAchieved", submission.playedAtUnixMillis},
         {"judgements", {{"pgreat", submission.pGreat},
                         {"great", submission.great},
                         {"good", submission.good},
                         {"bad", submission.bad},
                         {"poor", submission.poor}}},
         {"optional", {{"fast", submission.fast},
                       {"slow", submission.slow},
                       {"maxCombo", submission.maxCombo},
                       {"bp", submission.bad + submission.poor},
                       {"gauge", std::clamp(submission.finalGauge, 0.0F, 100.0F)}}}
    }})}
};
```

Map both assisted-easy ranks to `ASSIST CLEAR`; map the remaining ranks to `FAILED`, `EASY CLEAR`, `CLEAR`, `HARD CLEAR`, `EX HARD CLEAR`, and `FULL COMBO`.

- [ ] Assert in the test that a sentinel API key never appears in `payloadJson`, and parse the payload back to verify it contains one score and no extra judgement keys.

- [ ] Run and pass:

```bash
cmake --build cmake-build-debug --target tachi_batch_manual_tests -j 6
ctest --test-dir cmake-build-debug -R '^tachi_batch_manual_tests$' --output-on-failure
```

- [ ] Commit:

```bash
git add src/ir/tachi src/ir/CMakeLists.txt CMakeLists.txt tests/tachi_batch_manual_tests.cpp
git commit -m "feat: serialize Tachi batch manual scores"
```

---

### Task 3: Persist per-profile non-secret settings and device-local credentials

**Files:**
- Create: `src/ir/IrProfileSettings.h`
- Create: `src/ir/IrProfileSettings.cpp`
- Create: `src/ir/IrCredentialStore.h`
- Create: `src/ir/IrCredentialStore.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `src/AppSettings.h`
- Modify: `src/AppSettings.cpp`
- Modify: `src/AppSettingsStore.h`
- Modify: `src/AppSettingsStore.cpp`
- Modify: `src/PlayerProfile.h`
- Modify: `src/PlayerProfileManager.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/app_settings_store_tests.cpp`
- Modify: `tests/player_profile_manager_tests.cpp`
- Create: `tests/ir_credential_store_tests.cpp`

- [ ] Extend settings tests first. Require a default disabled `tachi` entry, `autoSubmit == false`, default origin `https://boku.tachi.ac`, schema-1 to schema-2 migration, round-trip persistence, URL sanitization, and proof that an API-key sentinel is absent from serialized settings.

- [ ] Add credential-store tests for missing file, save/load/replace/remove, malformed/future JSON, empty-key rejection, 4 KiB maximum key length, atomic-write failure rollback, and diagnostics that do not echo the key.

- [ ] Extend profile manager tests to require `PlayerProfilePaths::irCredentialsJson == root / "ir-credentials.json"` while keeping a newly created or duplicated profile's credential file absent.

- [ ] Run the three targets and observe failures:

```bash
cmake --build cmake-build-debug --target app_settings_store_tests ir_credential_store_tests player_profile_manager_tests -j 6
```

- [ ] Implement the settings model:

```cpp
struct IrProviderSettings {
  bool enabled = false;
  bool autoSubmit = false;
  std::string serverOrigin = "https://boku.tachi.ac";
  bool operator==(const IrProviderSettings &) const = default;
};
```

Store it under `settings.ir.providers.tachi`. Add `std::map<std::string, ir::IrProviderSettings> irProviders` to `AppSettings`. Increment `AppSettingsStore::kCurrentSchemaVersion` from 1 to 2 and add a version-1 migration that inserts the default object without altering existing settings.

- [ ] Implement `normalizeServerOrigin`. Accept only an absolute `http` or `https` origin with a nonempty host; reject user info, path other than `/`, query, fragment, controls, and strings over 2 KiB. Lowercase scheme/host, remove the trailing slash and default port, preserve an explicit nondefault port. On load, replace invalid stored values with the Bokutachi default and add a non-secret diagnostic.

- [ ] Implement `IrCredentialStore` using `versioned_json::saveAtomic` and this file shape:

```json
{
  "schemaVersion": 1,
  "providers": {
    "tachi": {
      "apiKey": "device-local-secret"
    }
  }
}
```

Expose `load(path)`, `save(path, credentials)`, `replaceApiKey(path, providerId, key)`, and `removeApiKey(path, providerId)`. Return only status plus sanitized diagnostics; never include key values in comparison messages.

- [ ] Add `irCredentialsJson` to `PlayerProfilePaths` and assign it in `makePathsAtRoot`. Do not create, migrate, snapshot, validate as required profile metadata, or copy this file in `buildProfile`.

- [ ] Register the new test and link `AtomicFile.cpp`/`VersionedJson.cpp` as used by the store. Run and pass:

```bash
ctest --test-dir cmake-build-debug -R '^(foundation_profile_settings|ir_credential_store_tests|foundation_profile_manager)$' --output-on-failure
```

- [ ] Commit:

```bash
git add src/ir src/AppSettings.* src/AppSettingsStore.* src/PlayerProfile.h src/PlayerProfileManager.cpp CMakeLists.txt tests/app_settings_store_tests.cpp tests/player_profile_manager_tests.cpp tests/ir_credential_store_tests.cpp
git commit -m "feat: store per-profile IR configuration"
```

---

### Task 4: Add the generic SQLite outbox and repository state transitions

**Files:**
- Modify: `src/ir/IrOutboxModels.h`
- Modify: `src/ir/IrOutboxModels.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositorySchema.cpp`
- Create: `src/repositories/ReplayRepositoryIrOutbox.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `tests/replay_repository_tests.cpp`

- [ ] Write repository tests for schema migration from version 5 to 6, clean creation, malformed-current-schema rejection, unique `(provider_id, attempt_id)`, stable payload/hash storage, and absence of a replay foreign key.

- [ ] Add tests for manual ready insertion, ready-row selection order, atomic claim, persisted request count/user-intent consumption, transient scheduling, auth blocking, permanent failure, deferred origin/job pairing, ongoing polling, success, seven-day purge, retry, retry-all, discard, counts, and stale-upload recovery.

- [ ] Include `ReplayRepositoryIrOutbox.cpp` in the main and every test target that compiles `ReplayRepository`. Run the repository test and confirm new cases fail:

```bash
cmake --build cmake-build-debug --target replay_repository_tests -j 6
ctest --test-dir cmake-build-debug -R '^replay_repository_tests$' --output-on-failure
```

- [ ] Increment `ReplayRepository::kCurrentSchemaVersion` to 6. Add an exact schema inspection and version-5 migration for:

```sql
CREATE TABLE ir_outbox (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  provider_id TEXT NOT NULL,
  attempt_id TEXT NOT NULL,
  chart_md5 TEXT,
  chart_sha256 TEXT NOT NULL,
  payload_json TEXT NOT NULL,
  state INTEGER NOT NULL,
  local_result_ready INTEGER NOT NULL DEFAULT 0,
  request_attempt_count INTEGER NOT NULL DEFAULT 0,
  consecutive_failure_count INTEGER NOT NULL DEFAULT 0,
  next_attempt_at_ms INTEGER,
  next_request_user_intent INTEGER NOT NULL DEFAULT 0,
  remote_job_id TEXT,
  remote_origin TEXT,
  last_error_code TEXT,
  last_error_message TEXT,
  created_at_ms INTEGER NOT NULL,
  updated_at_ms INTEGER NOT NULL,
  completed_at_ms INTEGER,
  UNIQUE(provider_id, attempt_id),
  CHECK (local_result_ready IN (0, 1)),
  CHECK (next_request_user_intent IN (0, 1)),
  CHECK ((remote_job_id IS NULL AND remote_origin IS NULL) OR
         (remote_job_id IS NOT NULL AND remote_origin IS NOT NULL))
);
CREATE INDEX idx_ir_outbox_due
  ON ir_outbox(local_result_ready, state, next_attempt_at_ms, id);
CREATE INDEX idx_ir_outbox_attempt
  ON ir_outbox(provider_id, attempt_id);
```

- [ ] Define `IrOutboxState` with stable integer encodings and repository DTOs in `IrOutboxModels.h`. Validate every decoded row: known state, bounded provider/attempt/hash/payload/error fields, valid paired remote fields, nonnegative counters/times, and legal state/remote combinations.

- [ ] Add these repository operations, each guarded by `profile_database_activity` and `impl_->sessionMutex`:

```cpp
IrOutboxInsertOutcome EnqueueReadyIrOutboxDraft(
    const ir::IrOutboxDraft &draft, bool userIntent);
IrOutboxReadOutcome LoadIrOutbox(std::string_view providerId,
                                 std::string_view attemptId);
IrOutboxBatchOutcome ListDueIrOutbox(std::int64_t nowMs,
                                     std::size_t limit = 64);
IrOutboxClaimOutcome ClaimIrOutbox(std::int64_t rowId,
                                   IrOutboxState expectedState,
                                   std::int64_t nowMs);
IrOutboxMutationOutcome ApplyIrOutboxDelivery(
    const ir::IrOutboxDeliveryUpdate &update);
IrOutboxMutationOutcome RetryIrOutbox(std::int64_t rowId, std::int64_t nowMs);
IrOutboxMutationOutcome RetryAllIrOutbox(std::string_view providerId,
                                         std::int64_t nowMs);
IrOutboxMutationOutcome UnblockIrOutbox(std::string_view providerId,
                                        std::int64_t nowMs);
IrOutboxMutationOutcome DiscardIrOutbox(std::int64_t rowId);
IrOutboxCounts CountIrOutbox(std::string_view providerId);
IrOutboxMutationOutcome RecoverStaleIrOutbox(std::int64_t nowMs);
IrOutboxMutationOutcome PurgeSucceededIrOutbox(std::int64_t olderThanMs);
bool ClearIrOutbox(std::string &errorMessage);
```

Claim with `UPDATE ... WHERE id = ? AND state = ? AND local_result_ready = 1`; set `uploading`, increment `request_attempt_count`, and clear `next_request_user_intent` only when `expectedState == pending`. Return the consumed flag in the claim so polls never receive user intent.

- [ ] Implement stale recovery in one transaction: `uploading` plus paired remote fields becomes `awaiting_remote_result`; other `uploading` rows become `pending`. Preserve payload, counters, retry time, job ID, and origin.

`UnblockIrOutbox` changes only `blocked_configuration` rows to due `pending` rows and preserves `next_request_user_intent`; credential replacement is not itself a new user-intent POST.

- [ ] Sanitize stored error fields with the shared 512-byte helper. Never persist raw response bodies or request headers.

- [ ] Run and pass:

```bash
cmake --build cmake-build-debug --target replay_repository_tests -j 6
ctest --test-dir cmake-build-debug -R '^replay_repository_tests$' --output-on-failure
```

- [ ] Commit:

```bash
git add src/ir/IrOutboxModels.* src/repositories/ReplayRepository* src/CMakeLists.txt CMakeLists.txt tests/replay_repository_tests.cpp
git commit -m "feat: add durable IR submission outbox"
```

---

### Task 5: Stage automatic drafts and activate them with local-score acknowledgement

**Files:**
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryRecords.cpp`
- Modify: `src/repositories/ReplayRepositoryIrOutbox.cpp`
- Modify: `src/ResultPersistenceCoordinator.h`
- Modify: `src/ResultPersistenceCoordinator.cpp`
- Modify: `tests/replay_repository_tests.cpp`
- Modify: `tests/result_persistence_coordinator_tests.cpp`
- Modify: `tests/result_persistence_integration_tests.cpp`

- [ ] Add failing repository tests proving replay rows, pending score, and all automatic drafts commit or roll back together. Include two providers, duplicate provider IDs, mismatched attempt IDs, malformed drafts, and an injected failure after draft insertion.

- [ ] Add failing coordinator tests proving local projection occurs before activation, a failed activation leaves the pending score intact, recovery repeats projection idempotently, and only a successful combined acknowledgement exposes the row to `ListDueIrOutbox`.

- [ ] Change staging and persistence signatures without adding an overload that can silently drop drafts:

```cpp
result_persistence::StageOutcome StageChartResult(
    const result_persistence::ChartResultAttempt &attempt,
    std::span<const ir::IrOutboxDraft> drafts);

SaveOutcome persist(const ChartResultAttempt &attempt,
                    std::span<const ir::IrOutboxDraft> drafts = {});
```

Update `result_persistence::Dependencies::stage` to accept the span.

- [ ] Inside the existing `BEGIN IMMEDIATE` staging transaction, validate unique provider IDs and insert automatic rows with `state = pending`, `local_result_ready = 0`, and `next_request_user_intent = 0`. For an idempotent restage, require every stored draft to match provider ID, attempt ID, hashes, payload, and creation time exactly.

- [ ] Replace `AcknowledgePendingChartScore` with `AcknowledgePendingChartScoreAndActivateIr`. In one transaction:

```sql
DELETE FROM pending_chart_score_writes
 WHERE attempt_id = ? AND replay_id = ?;
UPDATE ir_outbox
   SET local_result_ready = 1,
       next_attempt_at_ms = COALESCE(next_attempt_at_ms, ?),
       updated_at_ms = ?
 WHERE attempt_id = ? AND local_result_ready = 0;
```

If the pending row is already absent, verify the replay identity and that no matching outbox row remains inactive before returning `AlreadyAcknowledged`.

- [ ] Route both `Coordinator::persist` and `Coordinator::recoverAll` through the combined operation. Set `receipt.scorePending = false` only after it commits.

- [ ] Run all three focused tests:

```bash
cmake --build cmake-build-debug --target replay_repository_tests result_persistence_coordinator_tests result_persistence_integration_tests -j 6
ctest --test-dir cmake-build-debug -R '^(replay_repository_tests|result_persistence_coordinator_tests|result_persistence_integration_tests)$' --output-on-failure
```

- [ ] Commit:

```bash
git add src/repositories/ReplayRepository* src/ResultPersistenceCoordinator.* tests/replay_repository_tests.cpp tests/result_persistence_coordinator_tests.cpp tests/result_persistence_integration_tests.cpp
git commit -m "feat: activate IR work after local persistence"
```

---

### Task 6: Strip device-local IR work from duplicated and exported profile snapshots

**Files:**
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryIrOutbox.cpp`
- Modify: `src/PlayerProfileManager.cpp`
- Modify: `src/ProfileArchive.cpp`
- Modify: `tests/player_profile_manager_tests.cpp`
- Modify: `tests/profile_archive_tests.cpp`

- [ ] Add tests that seed source credentials plus pending/deferred/succeeded outbox rows, duplicate/export the profile, and assert: the source remains unchanged; the duplicate/imported snapshot has zero outbox rows; no credential file or key bytes exist in the duplicate/archive.

- [ ] Run the two profile targets and observe the new failures:

```bash
cmake --build cmake-build-debug --target player_profile_manager_tests profile_archive_tests -j 6
ctest --test-dir cmake-build-debug -R '^(foundation_profile_manager|foundation_profile_archive)$' --output-on-failure
```

- [ ] Add `ReplayRepository::ClearIrOutboxSnapshot(path, error)` as a static helper that opens only the supplied snapshot through the validated schema path, begins an immediate transaction, deletes `ir_outbox`, verifies the count is zero, commits, and closes it. The helper must never accept the live source path implicitly.

- [ ] In `PlayerProfileManager::buildProfile`, call the helper on `staging.replaysDb` only after the duplicate snapshot and schema initialization succeed. Do not touch `duplicateSource->replaysDb`.

- [ ] In `ProfileArchiveService::Export`, call the helper on `workspace / "replays.db"` after `snapshotSqliteDatabase` and before size checks/archive creation. During every import, call it again on `staging.replaysDb` after snapshot/schema migration and before finalizing the installed profile. This makes even an older or deliberately crafted compatible archive start with no delivery work.

- [ ] Keep `ir-credentials.json` out of archive members and manifest accounting. Extend archive validation tests to reject a crafted credential member as an unknown member rather than ignoring it.

- [ ] Run and pass the focused profile tests, then inspect one test archive listing for only the documented members.

- [ ] Commit:

```bash
git add src/repositories/ReplayRepository* src/PlayerProfileManager.cpp src/ProfileArchive.cpp tests/player_profile_manager_tests.cpp tests/profile_archive_tests.cpp
git commit -m "fix: exclude IR operational state from profiles"
```

---

### Task 7: Add bounded cancellable HTTP transport on every platform

**Files:**
- Create: `src/ir/IrHttpClient.h`
- Create: `src/ir/IrHttpClient.cpp`
- Create: `src/ir/IrHttpClientIOS.h`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `src/iOSNatives.mm`
- Modify: `CMakeLists.txt`
- Create: `tests/ir_http_client_tests.cpp`

- [ ] Write pure tests for request validation, case-insensitive safe-header lookup, response-body cap behavior, cancellation mapping, and rejection of redirects for authenticated requests. Use a fake transport seam; do not put bearer values in assertion messages.

- [ ] Define the transport contract:

```cpp
enum class IrHttpMethod { Get, Post };
enum class IrTransportError {
  None, Cancelled, Offline, Dns, Connect, Tls, Timeout, ResponseTooLarge, Other
};

struct IrHttpRequest {
  IrHttpMethod method = IrHttpMethod::Get;
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  std::size_t maximumResponseBytes = 1024 * 1024;
  std::chrono::seconds connectTimeout{10};
  std::chrono::seconds totalTimeout{25};
  bool followRedirects = false;
};

struct IrHttpResponse {
  long statusCode = 0;
  std::string body;
  std::optional<std::string> retryAfter;
  IrTransportError transportError = IrTransportError::None;
  std::string diagnostic;
};

class IrHttpClient {
public:
  virtual ~IrHttpClient() = default;
  virtual IrHttpResponse perform(const IrHttpRequest &,
                                 std::stop_token) noexcept = 0;
};
```

- [ ] Implement the desktop/Android path in `IrHttpClient.cpp` with `CurlRAII.h`: `CURLOPT_FOLLOWLOCATION = 0`, HTTP/HTTPS only, connect/total timeout, bounded write callback, `CURLOPT_XFERINFOFUNCTION` cancellation, and `ConfigureCurlTrustStore`. Capture only `Retry-After`; never log headers or body.

- [ ] Declare `PerformIrHttpRequestIOS` in `IrHttpClientIOS.h` and implement it inside `iOSNatives.mm` with an ephemeral `NSURLSession`. Use a session delegate that returns `nil` from redirection, enforce the response cap while receiving data, poll cancellation without blocking the main thread, and return HTTP status/body for driver classification instead of treating HTTP 4xx as a transport failure.

- [ ] Make `CreatePlatformIrHttpClient()` compile to the curl client outside iOS and the native bridge client on iOS. Keep `IrHttpClient.cpp` free of curl includes under the iOS preprocessor branch.

- [ ] Run and pass:

```bash
cmake --build cmake-build-debug --target ir_http_client_tests -j 6
ctest --test-dir cmake-build-debug -R '^ir_http_client_tests$' --output-on-failure
```

- [ ] Commit:

```bash
git add src/ir/IrHttpClient* src/ir/CMakeLists.txt src/iOSNatives.mm CMakeLists.txt tests/ir_http_client_tests.cpp
git commit -m "feat: add cancellable IR HTTP transport"
```

---

### Task 8: Implement Tachi submission, immediate responses, and deferred polling

**Files:**
- Create: `src/ir/tachi/TachiResponseParser.h`
- Create: `src/ir/tachi/TachiResponseParser.cpp`
- Create: `src/ir/tachi/TachiDriver.h`
- Create: `src/ir/tachi/TachiDriver.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `tests/tachi_driver_tests.cpp`

- [ ] Write response fixtures for immediate accepted score, accepted with warnings, converter rejection, malformed JSON, oversized diagnostics, 202 with valid `importID`, 202 with invalid/missing ID, ongoing poll, completed poll, and completed rejected import.

- [ ] Write fake-HTTP driver tests that assert exact method/path/header behavior, absence/presence of `X-User-Intent`, Bearer use from the current runtime configuration, 302 rejection, same-origin polling after configured origin changes, and no second POST for an awaiting row.

- [ ] Implement `TachiDriver` with provider ID `tachi` and capabilities:

```cpp
return {.readOnly = false,
        .chartRankings = true,
        .scoreSubmission = true,
        .deferredSubmission = true};
```

Delegate draft construction to `TachiBatchManual`.

- [ ] For submission, build exactly:

```text
POST <normalized-origin>/ir/direct-manual/import
Authorization: Bearer <current-key>
Content-Type: application/json
X-User-Intent: true
```

Add the last header only when the claim consumed user intent. Set the response cap to 1 MiB and `followRedirects = false`.

- [ ] Parse non-202 2xx Import Documents. One accepted score ID means `Succeeded`; converter errors without an accepted score mean `PermanentFailure`; mixed acceptance/warnings means success with a bounded sanitized diagnostic.

- [ ] Parse 202 as `Deferred`, validate `importID` as a bounded safe path segment, and return both that ID and the normalized request origin. Ignore any response-provided URL.

- [ ] Poll only with:

```text
GET <persisted-remote-origin>/api/v1/imports/<validated-importID>/poll-status
Authorization: Bearer <current-key>
```

Map `ongoing` to `Ongoing` and completed documents through the immediate parser. Never add `X-User-Intent` to a poll.

- [ ] Classify transport errors, 408, 429, and 5xx as transient; 401/403 as blocked configuration; other 4xx, 3xx, invalid 202 data, and malformed successful documents as permanent. Parse valid delta-seconds `Retry-After` into `DeliveryOutcome::retryAfterDelay`; the worker combines that duration with its injected clock.

- [ ] Run and pass:

```bash
cmake --build cmake-build-debug --target tachi_driver_tests -j 6
ctest --test-dir cmake-build-debug -R '^tachi_driver_tests$' --output-on-failure
```

- [ ] Commit:

```bash
git add src/ir/tachi src/ir/CMakeLists.txt CMakeLists.txt tests/tachi_driver_tests.cpp
git commit -m "feat: submit and poll Tachi imports"
```

---

### Task 9: Fetch and normalize Bokutachi chart rankings

**Files:**
- Create: `src/ir/tachi/TachiRankingParser.h`
- Create: `src/ir/tachi/TachiRankingParser.cpp`
- Modify: `src/ir/tachi/TachiDriver.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `tests/tachi_driver_tests.cpp`
- Create: `tests/tachi_ranking_parser_tests.cpp`
- Modify: `CMakeLists.txt`

- [x] Write native parser fixtures for identity, SHA-256 chart resolution,
  authentic and missing judgement timing, all BMS lamp strings, user mapping,
  page ordering, field bounds, and oversized responses.

- [x] Implement `TachiDriver::fetchChartRanking` with native BMS routes:

```text
POST <normalized-origin>/api/v1/games/bms-{7k|14k}/charts/resolve
GET  <normalized-origin>/api/v1/status
GET  <normalized-origin>/api/v1/games/bms-{7k|14k}/charts/<chartID>/pbs?startRanking=N
Authorization: Bearer <current-key>
```

Validate the resolved game, SHA-256, and note count. Use native server ranks,
page until `outOf` is satisfied, and reject changed or incomplete pagination.
Normalize native score, lamp, optional BP/combo/time, and authentic optional
`epg/lpg/egr/lgr`; do not use the beatoraja compatibility conversion.

- [x] Run and pass both tests:

```bash
cmake --build cmake-build-debug --target tachi_driver_tests tachi_ranking_parser_tests -j 6
ctest --test-dir cmake-build-debug -R '^(tachi_driver_tests|tachi_ranking_parser_tests)$' --output-on-failure
```

- [ ] Commit:

```bash
git add src/ir/tachi src/ir/CMakeLists.txt CMakeLists.txt tests/tachi_driver_tests.cpp tests/tachi_ranking_parser_tests.cpp
git commit -m "feat: fetch Bokutachi chart rankings"
```

---

### Task 10: Run durable submissions through a profile-bound worker

**Files:**
- Create: `src/ir/IrSubmissionService.h`
- Create: `src/ir/IrSubmissionService.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `tests/ir_submission_service_tests.cpp`

- [ ] Write a deterministic service harness with a temporary replay database, fake wall/monotonic clocks, fake credential lookup, fake HTTP, fake driver, and a completion barrier. Avoid real sleeping by injecting `waitUntil`/wake behavior.

- [ ] Add failing tests for startup stale recovery; disabled-provider pause/resume; auto-submit disablement leaving existing rows intact; read-only provider exclusion; missing key blocking without consuming user intent; key replacement wake; pending-row origin changes; deferred-row origin pinning; automatic POST; manual intent header; offline/408/429/5xx backoff; 202 persistence; ongoing polls; completion; permanent failure; manual retry while deferred; retry-all; success purge; and stop/profile pause during in-flight I/O.

- [ ] Implement `IrSubmissionService` ownership and configuration:

```cpp
struct IrActiveProfileConfig {
  std::string profileId;
  std::map<std::string, IrProviderSettings> providers;
};

class IrSubmissionService {
public:
  void start(IrActiveProfileConfig);
  void pauseAndCancel();
  void activateProfile(IrActiveProfileConfig);
  void setApplicationActive(bool active);
  void notifyConfigurationChanged();
  void notifyOutboxChanged();
  void stop();
  IrOutboxInsertOutcome enqueueManual(const IrOutboxDraft &draft);
  IrOutboxMutationOutcome retry(std::int64_t rowId);
  IrOutboxMutationOutcome retryAll(std::string_view providerId);
  IrOutboxMutationOutcome discard(std::int64_t rowId);
  IrOutboxCounts counts(std::string_view providerId) const;
  IrAttemptStatusSnapshot status(std::string_view providerId,
                                 std::string_view attemptId) const;
};
```

Use one `std::jthread`, a mutex, condition variable, stop source for the current HTTP request, and a bounded status map. All UI snapshots must be read without SQLite access.

- [ ] Worker loop rules:

1. Wait while stopped, app-inactive, profile-paused, or no enabled provider has due work.
2. Read a bounded due batch and skip providers that are missing, disabled, read-only, or submission-incapable.
3. Load the current key immediately before claim/send. Missing key moves the row to `blocked_configuration` without consuming intent.
4. Claim one row transactionally. Perform HTTP outside SQLite/profile database guards.
5. Persist the typed outcome only if the service is still on the same active profile generation.
6. Publish a bounded status snapshot and wake for the next due time.

- [ ] Implement the persisted backoff schedule by consecutive transient failures: 10 seconds, 30 seconds, 2 minutes, 10 minutes, then 1 hour for all later failures. Let a valid longer `Retry-After` extend the time. Reset failures after any valid protocol response; schedule `Ongoing` polls for 10 seconds.

- [ ] On `Deferred`, store job ID and request origin and move to `awaiting_remote_result`. On restart/retry, poll it and never POST it. On success, invoke a callback with profile/provider/origin/chart hashes for ranking-cache invalidation.

- [ ] On a credential replacement/removal notification, cancel any affected in-flight request and clear affected ranking state. Replacement calls `UnblockIrOutbox(providerId, now)` before waking the worker; removal leaves pending rows blockable and does not discard them.

- [ ] Purge only `succeeded` rows whose `completed_at_ms` is older than seven days. Never auto-delete blocked, failed, deferred, or pending rows.

- [ ] Catch exceptions at the thread boundary, store only a sanitized generic transient diagnostic, and continue running.

- [ ] Run and pass:

```bash
cmake --build cmake-build-debug --target ir_submission_service_tests -j 6
ctest --test-dir cmake-build-debug -R '^ir_submission_service_tests$' --output-on-failure
```

- [ ] Commit:

```bash
git add src/ir/IrSubmissionService.* src/ir/CMakeLists.txt CMakeLists.txt tests/ir_submission_service_tests.cpp
git commit -m "feat: process IR outbox in background"
```

---

### Task 11: Add cancellable ranking requests and five-minute memory caching

**Files:**
- Create: `src/ir/IrRankingService.h`
- Create: `src/ir/IrRankingService.cpp`
- Modify: `src/ir/IrRankingModels.h`
- Modify: `src/ir/IrRankingModels.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Create: `tests/ir_ranking_service_tests.cpp`

- [ ] Write tests with a fake monotonic clock, fake credential lookup, fake driver/HTTP, and controllable completion. Cover fetch-on-open, fresh-cache reuse, expiry, refresh bypass, error/not-found non-caching, latest-request wins, close cancellation, and late completion rejection.

- [ ] Test cache separation by profile ID, provider ID, normalized origin, key mode, SHA-256, and total notes. Inspect the key/value types and serialized debug output to prove no API key is stored.

- [ ] Add invalidation tests for profile switch, provider disable, origin change, credential change, shutdown, and successful submission of only the matching profile/provider/origin/chart.

- [ ] Define the request/cache boundaries:

```cpp
struct IrLocalComparison {
  std::string label;
  int score = 0;
  int maxScore = 0;
  int clearType = kClearTypeFailedRank;
  std::optional<int> badPoints;
  std::optional<int> maxCombo;
};

struct IrRankingRequest {
  std::uint64_t generation = 0;
  std::string profileId;
  std::string providerId;
  std::string serverOrigin;
  IrChartQuery chart;
  std::optional<IrLocalComparison> localComparison;
};
```

Keep `localComparison` in active request/snapshot state only; cache only the normalized remote `IrChartRanking`.

- [ ] Implement one `std::jthread`, one latest pending request, one per-request stop source, a generation counter, and a mutex-protected cache. Expose nonblocking `open`, `refresh`, `close`, `snapshot`, `invalidate`, `activateProfile`, and `stop` methods.

- [ ] Cache successful rankings for five minutes using the injected monotonic clock. Do not cache not-found, auth, unsupported, transient, malformed, oversized, or cancelled outcomes.

- [ ] Fetch the credential at request execution time. Missing key yields `AuthenticationRequired`; it does not disable the modal entry point.

- [ ] Run and pass:

```bash
cmake --build cmake-build-debug --target ir_ranking_service_tests -j 6
ctest --test-dir cmake-build-debug -R '^ir_ranking_service_tests$' --output-on-failure
```

- [ ] Commit:

```bash
git add src/ir/IrRanking* src/ir/CMakeLists.txt CMakeLists.txt tests/ir_ranking_service_tests.cpp
git commit -m "feat: cache cancellable IR ranking reads"
```

---

### Task 12: Wire service lifecycle, profile switching, and gameplay draft capture

**Files:**
- Modify: `src/context.h`
- Modify: `src/ProfileSessionCoordinator.h`
- Modify: `src/ProfileSessionCoordinator.cpp`
- Modify: `src/main.cpp`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/ResultScene.h`
- Modify: `tests/profile_switch_tests.cpp`
- Modify: `tests/result_persistence_integration_tests.cpp`
- Modify: `tests/application_result_recovery_tests.cpp`

- [ ] Extend profile-switch tests with fake profile-bound services. Require cancellation/pause before `SwitchGuard`/database rebinding, target activation after settings/input/repositories are ready, old-profile reactivation on every rollback path, and no update against the newly bound database from an old generation.

- [ ] Add an integration test where auto-submit is enabled, gameplay-equivalent code builds a Tachi draft, persistence commits it inactive, projection/ack activates it, and a manual-mode result produces no automatic row.

- [ ] Add `ApplicationContext` ownership in declaration order so services die before repositories:

```cpp
ir::IrDriverRegistry irDrivers;
std::unique_ptr<ir::IrHttpClient> irHttpClient;
std::unique_ptr<ir::IrRankingService> irRankingService;
std::unique_ptr<ir::IrSubmissionService> irSubmissionService;
```

Register one `TachiDriver`. Create/start services only after database initialization and local result recovery, not during the early constructor before schemas are ready.

- [ ] Extend `ProfileSessionDependencies` with `pauseProfileServices`, `activateProfileServices`, and `restoreProfileServices`. Validate and load the target profile/settings/input first, then call pause before taking the database switch guard. From that point, every early return must reactivate the old profile snapshot. Call target activation only after target settings/input/databases are ready; call old-profile restoration from rollback before returning. A pause failure returns `SwitchBlocked` without rebinding anything.

- [ ] Build active config from the active profile ID plus sanitized `AppSettings::irProviders`; configure credential lookup with `activePaths().irCredentialsJson`. On successful switch, clear ranking cache and wake stale/due outbox work for the new database.

- [ ] In the application foreground handler, call submission `setApplicationActive(!background)` and cancel ranking reads on background. On foreground, wake submission recovery/due work. Do not request mobile background execution entitlements.

- [ ] In `ApplicationContext::~ApplicationContext`, stop and reset ranking and submission services before `scoreRepository.Shutdown()` and `replayRepository.Shutdown()`.

- [ ] In `GamePlayScene::scheduleResultTransition`, after creating the immutable `ChartResultAttempt`, capture one `playedAtUnixMillis`, build `IrSubmission`, retain it in `ResultPersistenceOptions`, and build automatic drafts only for enabled, auto-submit, non-read-only, submission-capable drivers. Then call:

```cpp
context.resultPersistence.persist(*resultPersistenceOptions.attempt,
                                  automaticDrafts);
```

No API key or network call is permitted in this path.

- [ ] Start IR services from the successful post-recovery runtime callback in `main.cpp`. Keep local result recovery authoritative: an IR startup warning or offline state must not fail application startup.

- [ ] Run and pass:

```bash
cmake --build cmake-build-debug --target profile_switch_tests result_persistence_integration_tests application_result_recovery_tests -j 6
ctest --test-dir cmake-build-debug -R '^(foundation_profile_switch|result_persistence_integration_tests|application_result_recovery_tests)$' --output-on-failure
```

- [ ] Commit:

```bash
git add src/context.h src/ProfileSessionCoordinator.* src/main.cpp src/scene/play/GamePlayScene.cpp src/scene/ResultScene.h tests/profile_switch_tests.cpp tests/result_persistence_integration_tests.cpp tests/application_result_recovery_tests.cpp
git commit -m "feat: bind IR services to profiles and gameplay"
```

---

### Task 13: Add the capability-aware IR settings tab

**Files:**
- Create: `src/ir/IrSettingsPresentation.h`
- Create: `src/ir/IrSettingsPresentation.cpp`
- Create: `src/scene/SettingsSceneIr.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `src/scene/CMakeLists.txt`
- Modify: `src/scene/SettingsScene.h`
- Modify: `src/scene/SettingsSceneLayout.cpp`
- Modify: `src/scene/SettingsSceneControls.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/ir_settings_presentation_tests.cpp`

- [ ] Write pure presentation tests for read/write Bokutachi and a read-only fake. Require enablement and read settings for both, but auto-submit/queue/retry/discard controls only for the read/write provider.

- [ ] Test store-first actions with injected failures: enable, auto-submit, origin edit, replace key, remove key, retry-all, and discard. A failed save must leave both the visible model and active service configuration unchanged.

- [ ] Implement `IrSettingsPresentation` from capabilities, provider settings, credential presence, and `IrOutboxCounts`. It must contain only `hasCredential`, never credential contents.

- [ ] Add `SettingsTab::Ir`, tab button/text members, `buildIrTab`, and `refreshIrSettingsPresentation`. Add `SettingsSceneIr.cpp` to `src/scene/CMakeLists.txt`. The existing tab rail is already a `ScrollView`; verify the eleventh tab remains reachable at compact height.

- [ ] Build the IR tab with provider card(s): Enabled, Auto Submit, Server Origin, masked key state with Replace/Remove, pending/awaiting/blocked/failed counts, Retry All Now, discard confirmation, and the device-local/export-exclusion note. Label HTTP origins as insecure.

- [ ] For settings changes, copy the prior value, perform one atomic store, then update `context.settings` and notify services only on success. For key changes, update only `ir-credentials.json`, clear affected ranking cache, and wake blocked submission rows. Do not attempt a cross-file transaction.

- [ ] Keep typed key text in the editor only until save/cancel. Existing keys display a fixed mask and are never loaded into a `TextView` or diagnostic.

- [ ] Refresh counts from the submission service's status/count snapshot rather than querying SQLite each frame. Route Retry All/Discard through service methods that wake the worker after mutation.

- [ ] Run and pass:

```bash
cmake --build cmake-build-debug --target ir_settings_presentation_tests main -j 6
ctest --test-dir cmake-build-debug -R '^ir_settings_presentation_tests$' --output-on-failure
```

- [ ] Commit:

```bash
git add src/ir/IrSettingsPresentation.* src/scene/SettingsScene* src/ir/CMakeLists.txt src/scene/CMakeLists.txt CMakeLists.txt tests/ir_settings_presentation_tests.cpp
git commit -m "feat: add profile IR settings controls"
```

---

### Task 14: Add provider-neutral result submission status and manual actions

**Files:**
- Create: `src/ir/IrResultPresentation.h`
- Create: `src/ir/IrResultPresentation.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/ir_result_presentation_tests.cpp`
- Modify: `tests/view_layout_tests.cpp`

- [ ] Write pure status mapping tests for Not submitted, Queued, Submitting, Waiting, Submitted, Authentication required, Failed, and Unsupported. Require Submit only when `SaveOutcome::saved()` is true and no row exists, Retry only for retryable/blocked/permanent rows, and no IR control while the local score is pending or local persistence still requires a decision.

- [ ] Add result layout tests showing IR failure never hides Back/Retry/Export actions and never participates in `persistenceDecisionRequired`.

- [ ] Implement `IrResultPresentation` from driver capabilities, provider settings, local save outcome, retained canonical submission, and the service's attempt-status snapshot. Keep provider display text outside repository models.

- [ ] Extend `ResultPersistenceOptions` with `std::shared_ptr<const ir::IrSubmission> irSubmission`. After the local save reaches `SaveState::Saved`, build a compact status block adjacent to `resultActions`; do not place it inside the existing blocking local-persistence panel.

- [ ] Implement manual Submit by calling `TachiDriver::buildDraft` through the registry, then `IrSubmissionService::enqueueManual(draft)`, which inserts a ready row with user intent and wakes the worker. It must reuse the retained captured timestamp and must not rebuild from mutable chart files.

- [ ] Implement Retry by row identity. For `awaiting_remote_result`, make it due immediately without clearing the remote job/origin. For other retryable states, return it to pending and set the next POST's user intent.

- [ ] Poll the service's thread-safe status snapshot during `ResultScene::update` and rebuild text/buttons only when the snapshot revision changes. Never query SQLite each frame.

- [ ] Run and pass:

```bash
cmake --build cmake-build-debug --target ir_result_presentation_tests view_layout_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(ir_result_presentation_tests|view_layout_tests)$' --output-on-failure
```

- [ ] Commit:

```bash
git add src/ir/IrResultPresentation.* src/ir/CMakeLists.txt src/scene/ResultScene.* CMakeLists.txt tests/ir_result_presentation_tests.cpp tests/view_layout_tests.cpp
git commit -m "feat: show IR submission controls on results"
```

---

### Task 15: Build the shared rankings modal and connect both scene entry points

**Files:**
- Create: `src/ir/IrRankingModal.h`
- Create: `src/ir/IrRankingModal.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/ir_ranking_modal_tests.cpp`
- Modify: `tests/view_layout_tests.cpp`

- [ ] Write modal-model tests for loading, success, empty/not-found, auth, transient/offline, malformed, oversized, cancelled, refresh, and retry states. Require the comparison card to remain separate and the `You` entry highlighted.

- [ ] Add virtualization and responsive layout tests: a 20,000-entry result must create only visible `RecyclerView` rows; wide rows show rank/player/EX rate/lamp/BP/combo; compact rows keep rank/player/rate/lamp and expand details on selection.

- [ ] Implement `IrRankingModal` as a safe-area-aware scrim plus centered panel presented through an `OverlayPortal`. It owns no driver/HTTP state; it opens/refreshes/closes a generation in `IrRankingService` and renders immutable snapshots.

- [ ] Add header, fetch time, Refresh, Close, comparison card, and `RecyclerView<IrChartRankingEntry>`. Format EX rate as `score / maxScore * 100`, handle absent BP/combo/time as an em dash, and use canonical lamp labels/colors.

- [ ] Close on explicit button, scrim, Back/Escape, or platform back. Closing calls ranking service cancellation and removes the overlay without waiting for the HTTP thread.

- [ ] Redesign the main-menu right rail into a vertically scrollable content group so Start, Rankings, Viewer/Reveal, Records, and archive-specific actions remain reachable at compact heights. Add a full-width `Rankings` slot below Start and above secondary chart actions; preserve the existing 300-pixel rail and 220-pixel action width.

- [ ] Enable the main-menu Rankings button only for enabled Bokutachi, ranking capability, key mode 7/14, positive notes, and valid SHA-256. Do not require a local file or key. On click, snapshot the selected `ChartMetaRecord` and current local PB, then open the modal. Do not fetch on selection changes.

- [ ] Add an `OverlayPortal` to the result root and a Rankings button in the wrapping non-course single-chart action row. Build its query directly from the completed chart and a `This Play` comparison from `RhythmState`; do not require a retained submission or local-persistence eligibility. Supported autoplay, practice, and replay results may view rankings even though they cannot submit. Opening rankings must not submit the score.

- [ ] Guard modal completions with generation and full request identity. Switching charts/profiles, disabling provider, changing key/origin, leaving the scene, or closing the modal must prevent late data from replacing current UI.

- [ ] Run and pass:

```bash
cmake --build cmake-build-debug --target ir_ranking_modal_tests view_layout_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(ir_ranking_modal_tests|view_layout_tests)$' --output-on-failure
```

- [ ] Commit:

```bash
git add src/ir/IrRankingModal.* src/ir/CMakeLists.txt src/scene/MainMenuScene.* src/scene/ResultScene.* CMakeLists.txt tests/ir_ranking_modal_tests.cpp tests/view_layout_tests.cpp
git commit -m "feat: add shared Bokutachi rankings modal"
```

---

### Task 16: Verify durability, security, desktop, and mobile builds

**Files:**
- Modify only files required by failures found in this task.

- [ ] Run every focused IR and touched-foundation test together:

```bash
cmake --build cmake-build-debug --target \
  ir_driver_tests \
  tachi_batch_manual_tests \
  ir_credential_store_tests \
  replay_repository_tests \
  result_persistence_coordinator_tests \
  result_persistence_integration_tests \
  ir_http_client_tests \
  tachi_driver_tests \
  tachi_ranking_parser_tests \
  ir_submission_service_tests \
  ir_ranking_service_tests \
  ir_settings_presentation_tests \
  ir_result_presentation_tests \
  ir_ranking_modal_tests \
  profile_switch_tests \
  player_profile_manager_tests \
  profile_archive_tests \
  view_layout_tests -j 6

ctest --test-dir cmake-build-debug -R \
  '^(ir_|tachi_|replay_repository_tests|result_persistence_|foundation_profile_switch|foundation_profile_manager|foundation_profile_archive|view_layout_tests)' \
  --output-on-failure
```

Expected: all selected tests pass.

- [ ] Run the complete desktop suite and main build:

```bash
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: `main` builds and the full suite passes.

- [ ] Run credential-hygiene inspections. Use a sentinel from the tests, not a real key:

```bash
rg -n 'device-local-secret|sentinel-api-key' src tests docs/superpowers/plans/2026-07-18-bokutachi-ir.md
rg -n 'Authorization|apiKey|api_key' src/ir src/repositories src/ProfileArchive.cpp src/PlayerProfileManager.cpp
```

Expected: the sentinel occurs only in tests/this plan; production matches are confined to credential loading and in-memory HTTP header assembly. No queue SQL, diagnostic formatter, log call, cache model, or archive code includes a credential value.

- [ ] Re-run the outbox/archive tests under AddressSanitizer if the existing CMake configuration exposes the sanitizer option; otherwise record that the standard debug suite was used. Do not introduce a new project-wide sanitizer configuration in this feature.

- [ ] Run the iOS build-only path; this does not archive, sign, or upload:

```bash
scripts/ios_firebase_deploy.sh --build-only
```

Expected: the iOS target compiles the `NSURLSession` IR bridge and all synchronized sources successfully.

- [ ] With Android signing environment configured, run the Android build-only path:

```bash
scripts/android_firebase_deploy.sh --build-only
```

Expected: the Firebase release APK compiles and links libcurl without uploading. If the private signing environment is unavailable, record that exact environment limitation and still run:

```bash
scripts/android_firebase_deploy.sh --build-only --variant playDebug
```

- [ ] Check repository state and inspect the final diff for accidental generated files, private `.env` files, build output, credentials, or unrelated edits:

```bash
git status --short
git diff develop...HEAD --stat
git log --oneline develop..HEAD
```

- [ ] If verification required a code fix, commit it with a precise message and rerun the affected command. Otherwise leave no uncommitted files.

## Final Acceptance Checklist

- [ ] Automatic drafts commit with replay and pending local score, and are claimable only after combined local acknowledgement/activation.
- [ ] Offline/crash recovery retains pending work; stale deferred uploads resume by polling instead of POSTing again.
- [ ] 202 is never reported as success; the job ID and original validated origin persist as a pair.
- [ ] Manual Submit/Retry uses the current profile key and stable captured payload/time.
- [ ] Read-only capabilities are enforced even with a malicious or malformed fake driver.
- [ ] Credentials are absent from queue rows, payloads, settings, caches, archives, duplicates, diagnostics, and logs.
- [ ] Bokutachi ranking order/ranks match the agreed Tachi tuple, including BP descending and competition ties.
- [ ] Rankings fetch only when the modal opens or refreshes; both scenes use the same modal and keep local comparison outside remote rank.
- [ ] Profile switching, backgrounding, scene close, and shutdown cancel work without blocking UI or updating the wrong profile.
- [ ] No LR2IR implementation is included.
