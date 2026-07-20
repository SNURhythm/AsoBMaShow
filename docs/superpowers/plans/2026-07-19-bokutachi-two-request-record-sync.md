# Bokutachi Two-Request Record Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reconcile the active profile against its complete Bokutachi BMS score history using exactly two authenticated requests, persist an atomic nullable remote mirror, repair durable upload receipts, and project remote best score/lamp data into song select.

**Architecture:** Add a provider-neutral user-score snapshot contract to the IR driver boundary. `TachiDriver` fetches `/bms-7k/scores/all` and `/bms-14k/scores/all` once each, validates both envelopes into bounded nullable models, and hands the complete snapshot to a pure reconciliation planner. `IrSubmissionService` serializes this manual operation with outbox delivery; `ReplayRepository` replaces the origin-scoped mirror and applies receipt repairs in one transaction only after both responses validate.

**Tech Stack:** C++23, nlohmann/json, SQLite, SDL2/Yoga views, CMake/CTest.

## Global Constraints

- Every accepted manual sync performs exactly two HTTP requests in this order: `bms-7k`, then `bms-14k`. Do not inspect local charts or configured playtypes to skip one.
- Both requests use `GET /api/v1/users/me/games/{game}/scores/all` with the active profile's Bearer credential.
- Finish the second request even if the first returns an expected no-game-profile 404; cancellation is the only reason to stop before request two.
- A Tachi 404 counts as an empty game snapshot only when its valid API envelope identifies the missing user-game profile. An unknown or malformed 404 fails the whole sync.
- There is no automatic retry. Repeated taps coalesce while running and are rejected during a 60-second monotonic cooldown.
- Parse and validate both responses completely before opening the database mutation. Any transport, authentication, size, schema, referential-integrity, or storage error preserves the previous mirror and receipts.
- API keys and raw HTTP bodies never enter models, diagnostics, SQLite, JSON cache files, logs, or UI state.
- Remote optional data stays optional. Do not synthesize judgements, KPOOR, replay events, timing, gauge, LN mode, or provenance.
- The existing bounded `bokutachi-cache.json` remains the chart/user lookup cache. Full score history belongs in the profile replay SQLite database and must not expand that JSON file.
- A remote BMS score has no trustworthy AsoBMaShow LN-mode identity. Score/lamp projection is chart-hash-wide and is offered to all local LN-mode views, never represented as a locally verified ruleset score.
- Follow red-green TDD and commit after each independently passing task. Do not deploy.

---

## File Map

- Create `src/ir/IrRemoteScoreModels.h` and `.cpp`: bounded nullable snapshot and stored-score types.
- Create `src/ir/tachi/TachiUserScoreParser.h` and `.cpp`: Tachi envelope, relation, and optional-field parsing.
- Modify `src/ir/IrDriver.h` and `.cpp`: advertise reconciliation and expose the snapshot operation.
- Modify `src/ir/tachi/TachiDriver.h` and `.cpp`: perform exactly the two required requests.
- Create `src/ir/IrScoreReconciliation.h` and `.cpp`: pure receipt-repair planner.
- Modify `src/ir/IrSubmissionService.h` and `.cpp`: serialize manual sync and publish revisioned progress.
- Modify `src/repositories/ReplayRepository.h`, `ReplayRepositorySchema.cpp`, and `ReplayRepositoryIrOutbox.cpp`: schema version 10 and atomic snapshot application.
- Create `src/repositories/ReplayRepositoryIrRemoteScores.cpp`: bounded mirror reads and transactional replacement.
- Create `src/ir/IrScoreHistoryProjection.h` and `.cpp`: merge remote best score/lamp into existing caches.
- Modify `src/scene/MainMenuScene.h` and `.cpp`: apply the projection and refresh it after sync.
- Modify `src/ir/IrSettingsPresentation.h` and `.cpp` plus `src/scene/SettingsSceneIr.cpp`: expose `Sync IR records` and progress.
- Modify `src/CMakeLists.txt`, `src/ir/CMakeLists.txt`, and root `CMakeLists.txt`.
- Create `tests/ir_remote_score_models_tests.cpp`, `tests/tachi_user_score_parser_tests.cpp`, `tests/ir_score_reconciliation_tests.cpp`, and `tests/ir_score_history_projection_tests.cpp`.
- Modify `tests/tachi_driver_tests.cpp`, `tests/replay_repository_tests.cpp`, `tests/ir_submission_service_tests.cpp`, and `tests/ir_settings_presentation_tests.cpp`.

---

### Task 1: Define a Bounded, Nullable Remote Score Contract

**Files:**
- Create: `src/ir/IrRemoteScoreModels.h`
- Create: `src/ir/IrRemoteScoreModels.cpp`
- Create: `tests/ir_remote_score_models_tests.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
namespace ir {
enum class IrUserScoreSnapshotStatus {
  Succeeded,
  AuthenticationRequired,
  TransientFailure,
  MalformedResponse,
  OversizedResponse,
  Unsupported,
  Cancelled,
};

struct IrRemoteJudgements {
  std::optional<int> pGreat;
  std::optional<int> great;
  std::optional<int> good;
  std::optional<int> bad;
  std::optional<int> poor;
  [[nodiscard]] bool complete() const noexcept;
};

struct IrRemoteTimingBreakdown {
  std::optional<int> earlyPGreat, latePGreat;
  std::optional<int> earlyGreat, lateGreat;
  std::optional<int> earlyGood, lateGood;
  std::optional<int> earlyBad, lateBad;
  std::optional<int> earlyPoor, latePoor;
};

struct IrRemoteScore {
  std::int64_t remoteUserId = 0;
  std::string game;
  std::string remoteScoreId;
  std::string remoteChartId;
  std::string chartMd5;
  std::string chartSha256;
  std::string title;
  std::string artist;
  std::string service;
  std::optional<std::string> difficulty;
  std::optional<std::string> level;
  std::optional<double> levelNumber;
  int noteCount = 0;
  int score = 0;
  int lampRank = 0;
  std::optional<std::int64_t> timeAchievedUnixMillis;
  std::int64_t timeAddedUnixMillis = 0;
  IrRemoteJudgements judgements;
  IrRemoteTimingBreakdown timing;
  std::optional<int> fast, slow, maxCombo, badPoints;
  std::optional<float> finalGauge;
  std::vector<std::optional<float>> gaugeHistory;
  std::optional<std::string> random, gauge, inputDevice, client;
};

struct IrUserScoreSnapshot {
  std::vector<IrRemoteScore> scores;
};

struct IrUserScoreSnapshotOutcome {
  IrUserScoreSnapshotStatus status =
      IrUserScoreSnapshotStatus::MalformedResponse;
  std::optional<IrUserScoreSnapshot> snapshot;
  std::string code;
  std::string diagnostic;
};
}
```

- [ ] **Step 1: Write failing validation tests**

Test supported games, positive user IDs, lower-case hashes, bounded remote IDs/text, nullable difficulty/level, finite level number, non-negative metrics, score not exceeding `noteCount * 2`, known lamp rank, nullable timestamp/optional fields, finite gauge values in `[0, 100]`, and bounded gauge history. Test that missing judgement entries remain `nullopt`, while an explicit zero remains present. Reject control characters, duplicate remote score IDs within a snapshot, invalid hashes, overflow, NaN, and impossible negative metrics.

- [ ] **Step 2: Register and run the test for RED**

```bash
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target ir_remote_score_models_tests -j 6
```

Expected: compile failure because the types do not exist.

- [ ] **Step 3: Implement provider-neutral validation**

Use named constants for collection, string, and gauge-history limits. Validation returns a bounded diagnostic and never serializes the score or any credential. Keep BMS field names out of this layer so later read-only drivers can reuse the contract.

- [ ] **Step 4: Verify GREEN and commit**

```bash
cmake --build cmake-build-debug --target ir_remote_score_models_tests -j 6
./cmake-build-debug/ir_remote_score_models_tests
git diff --check
git add src/ir/IrRemoteScoreModels.* src/ir/CMakeLists.txt \
  tests/ir_remote_score_models_tests.cpp CMakeLists.txt
git commit -m "feat: define nullable IR remote score snapshots"
```

---

### Task 2: Parse Complete Tachi User-Score Responses

**Files:**
- Create: `src/ir/tachi/TachiUserScoreParser.h`
- Create: `src/ir/tachi/TachiUserScoreParser.cpp`
- Create: `tests/tachi_user_score_parser_tests.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
namespace ir::tachi {
struct ParsedUserGameScores {
  std::vector<IrRemoteScore> scores;
};

[[nodiscard]] IrUserScoreSnapshotOutcome parseUserGameScores(
    std::string_view expectedGame, long httpStatus,
    std::string_view responseBody);
}
```

- [ ] **Step 1: Add realistic failing fixtures**

Use the official response shape `success/body/{charts,scores,songs}`. Cover relation lookup by `chartID` and song ID, primary-chart enforcement, expected-game equality, MD5/SHA256 extraction, title/artist, difficulty/level/level number, note count, score/lamp, time fields, service/client metadata, all five judgements, all ten early/late values, fast/slow/maxCombo/BP/final gauge/gauge history, and explicit JSON nulls.

Add malformed tests for duplicate chart/song/score IDs, score referring to an absent chart, chart referring to an absent song, a non-primary chart, mixed game, unknown lamp, inconsistent score/user identity, oversized arrays/strings/history, and non-finite or out-of-range numeric conversions.

Add two 404 fixtures: a valid `success:false` Tachi envelope whose bounded `description` exactly ends with ` has not played {expectedGame}` yields `Succeeded` with an empty vector; any other 404, wrong-game suffix, missing description, or malformed error envelope yields `MalformedResponse`. This matches the official `withUserGameProfile` error without depending on the user's mutable username.

- [ ] **Step 2: Run for RED**

```bash
cmake --build cmake-build-debug --target tachi_user_score_parser_tests -j 6
```

Expected: missing parser failures.

- [ ] **Step 3: Implement strict relation-first parsing**

Parse the envelope with non-throwing JSON handling. Build bounded ID maps for songs and charts, then parse scores; do not retain raw JSON. Accept `timeAchieved: null`; require positive `timeAdded`. Map Tachi BMS lamp enum values through one tested function. Read optional metric keys only when present and type-correct. Do not compute KPOOR or fill absent judgement/timing values with zero.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target tachi_user_score_parser_tests -j 6
./cmake-build-debug/tachi_user_score_parser_tests
git diff --check
git add src/ir/tachi/TachiUserScoreParser.* src/ir/CMakeLists.txt \
  tests/tachi_user_score_parser_tests.cpp CMakeLists.txt
git commit -m "feat: parse Bokutachi user score snapshots"
```

---

### Task 3: Add the Exactly-Two-Request Driver Operation

**Files:**
- Modify: `src/ir/IrDriver.h`
- Modify: `src/ir/IrDriver.cpp`
- Modify: `src/ir/tachi/TachiDriver.h`
- Modify: `src/ir/tachi/TachiDriver.cpp`
- Modify: `tests/tachi_driver_tests.cpp`

**Interfaces:**

```cpp
struct IrDriverCapabilities {
  bool readOnly = false;
  bool chartRankings = false;
  bool scoreSubmission = false;
  bool deferredSubmission = false;
  bool scoreReconciliation = false;
};

using IrUserScoreProgress =
    std::function<void(std::string_view game, int completed, int total)>;

virtual IrUserScoreSnapshotOutcome fetchUserScoreSnapshot(
    const IrProviderRuntimeConfig &, IrHttpClient &, std::stop_token,
    IrUserScoreProgress) const;
```

Add the matching `IrDriverRegistry` forwarding method.

- [ ] **Step 1: Write capability and request-contract tests**

Assert Tachi advertises reconciliation. Capture requests and assert exactly two GETs, in 7K/14K order, with `/api/v1/users/me/games/.../scores/all`, Bearer authorization, no body, redirects disabled, and a named 64 MiB per-response maximum. Prove the key never appears in the returned diagnostic.

Test: two successes with one consistent positive `userID` merge; user-ID disagreement across games fails; expected 7K 404 plus 14K success merges; expected 404 for both succeeds empty; any first non-cancellation failure (including 401, malformed, oversized, timeout, or 500) still performs the second request then returns failure without a partial snapshot; cancellation during request one stops before request two; cancellation after request one prevents persistence at the service layer. With a real temporary `BokutachiCacheStore`, assert successful snapshots remember the user ID and bounded `(origin, game, SHA256) -> chartID` mappings, while an unavailable optional cache does not invalidate an otherwise complete snapshot.

- [ ] **Step 2: Run for RED**

```bash
cmake --build cmake-build-debug --target tachi_driver_tests -j 6
./cmake-build-debug/tachi_driver_tests
```

- [ ] **Step 3: Implement the driver method**

Normalize the origin and validate the credential once. Perform each request once with no retry. Record the first failure but, unless cancelled, perform and validate the second request. Merge only when both outcomes succeeded. Reject duplicate `remoteScoreId` values across the combined games. After the merged snapshot validates, best-effort remember its consistent user ID and chart mappings in the existing bounded `bokutachi-cache.json`; the SQLite mirror remains authoritative if this optional cache write is unavailable. Call progress before/after each request without exposing the URL or credential.

Update `validateCapabilities`: a read-only driver may reconcile and rank but may not submit; a writable driver may expose all capabilities.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target tachi_driver_tests -j 6
./cmake-build-debug/tachi_driver_tests
git diff --check
git add src/ir/IrDriver.* src/ir/tachi/TachiDriver.* tests/tachi_driver_tests.cpp
git commit -m "feat: fetch Bokutachi scores in exactly two requests"
```

---

### Task 4: Persist an Atomic Origin-Scoped Remote Mirror

**Files:**
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositorySchema.cpp`
- Create: `src/repositories/ReplayRepositoryIrRemoteScores.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/replay_repository_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
namespace ir {
struct IrRemoteSnapshotMutation {
  std::string providerId;
  std::string serverOrigin;
  std::int64_t synchronizedAtUnixMillis = 0;
  std::vector<IrRemoteScore> scores;
  std::vector<IrSubmissionReceipt> upsertedReceipts;
  std::vector<std::int64_t> deletedReceiptIds;
  std::vector<std::int64_t> settledOutboxRowIds;
  std::vector<std::int64_t> purgedSucceededOutboxRowIds;
};

struct IrRemoteSnapshotApplyOutcome {
  enum class Status { Applied, Invalid, StorageFailure };
  Status status = Status::StorageFailure;
  int remoteScoreCount = 0;
  int remoteScoresAdded = 0;
  int remoteScoresRemoved = 0;
  int receiptsUpserted = 0;
  int receiptsDeleted = 0;
  int outboxRowsSettled = 0;
  int ambiguousReceiptsPreserved = 0;
  std::string diagnostic;
};
}
```

Repository additions:

```cpp
ir::IrRemoteSnapshotApplyOutcome
ApplyIrRemoteSnapshot(const ir::IrRemoteSnapshotMutation &mutation);
std::vector<ir::IrRemoteScore> ListIrRemoteScores(
    std::string_view providerId, std::string_view serverOrigin);
ir::IrOutboxMutationOutcome ClearIrRemoteScores(
    std::string_view providerId, std::string_view serverOrigin);
ir::IrOutboxMutationOutcome ClearIrAccountEvidence(
    std::string_view providerId, std::string_view serverOrigin);
```

- [ ] **Step 1: Write schema version 10 tests**

Test fresh creation and version-9 migration for this logical `ir_remote_scores` schema, keyed by `(provider_id, server_origin, remote_score_id)`:

```sql
CREATE TABLE ir_remote_scores (
  provider_id TEXT NOT NULL,
  server_origin TEXT NOT NULL,
  remote_score_id TEXT NOT NULL,
  remote_user_id INTEGER NOT NULL,
  game TEXT NOT NULL,
  remote_chart_id TEXT NOT NULL,
  chart_md5 TEXT NOT NULL,
  chart_sha256 TEXT NOT NULL,
  title TEXT NOT NULL,
  artist TEXT NOT NULL,
  difficulty TEXT,
  level TEXT,
  level_number REAL,
  note_count INTEGER NOT NULL,
  score INTEGER NOT NULL,
  lamp_rank INTEGER NOT NULL,
  service TEXT NOT NULL,
  time_achieved_ms INTEGER,
  time_added_ms INTEGER NOT NULL,
  pgreat INTEGER, great INTEGER, good INTEGER, bad INTEGER, poor INTEGER,
  early_pgreat INTEGER, late_pgreat INTEGER,
  early_great INTEGER, late_great INTEGER,
  early_good INTEGER, late_good INTEGER,
  early_bad INTEGER, late_bad INTEGER,
  early_poor INTEGER, late_poor INTEGER,
  fast INTEGER, slow INTEGER, max_combo INTEGER, bad_points INTEGER,
  final_gauge REAL,
  gauge_history_json TEXT,
  random_mode TEXT, gauge_mode TEXT, input_device TEXT, client TEXT,
  sync_generation INTEGER NOT NULL,
  PRIMARY KEY(provider_id, server_origin, remote_score_id),
  CHECK(game IN ('bms-7k', 'bms-14k')),
  CHECK(remote_user_id > 0),
  CHECK(note_count >= 0 AND score >= 0 AND score <= note_count * 2),
  CHECK(time_added_ms > 0)
)
```

All optional count columns also receive non-negative checks, `final_gauge` receives `[0,100]`, and application validation enforces normalized origin, hashes, enum bounds, finite level/gauge values, and string/JSON size bounds. Store gauge history as bounded canonical JSON because it is the only variable-length numeric series; verify it decodes to nullable points and never exceeds its model bound. Add indexes for `(provider_id, server_origin, chart_sha256)` and `(provider_id, server_origin, remote_chart_id)`.

The table includes `sync_generation INTEGER NOT NULL`, and constraints for supported game, non-negative metrics, boolean-free nullable values, and normalized hashes. Add exact-schema, future-version, and malformed-current-schema failures.

- [ ] **Step 2: Write transaction tests**

Seed an old mirror, receipts, and duplicate pending/succeeded outbox rows, then apply a new snapshot. Assert exact origin-scoped replacement, no changes to another origin/provider, nullable values round-trip, matching non-active duplicate work is settled, compatible retained-success rows are purged only after receipt resolution, and a monotonically selected generation. Test `ClearIrAccountEvidence` deletes remote scores and receipts for exactly one provider/origin in one transaction and rolls both back on either failure. Inject failures during remote insert, receipt upsert/deletion, and outbox settlement; every case rolls back the entire mutation. Assert no raw response or credential column exists.

- [ ] **Step 3: Run for RED**

```bash
cmake --build cmake-build-debug --target replay_repository_tests -j 6
./cmake-build-debug/replay_repository_tests
```

- [ ] **Step 4: Implement schema and transaction**

Increment `ReplayRepository::kCurrentSchemaVersion` to 10. `BEGIN IMMEDIATE`, write the new generation, delete older generations for only the active provider/origin, apply the already-computed receipt changes, then commit. Validate the complete mutation before `BEGIN`. Do not touch canonical local `scores` rows or replay payloads.

- [ ] **Step 5: Verify and commit**

```bash
cmake --build cmake-build-debug --target replay_repository_tests -j 6
./cmake-build-debug/replay_repository_tests
git diff --check
git add src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositorySchema.cpp \
  src/repositories/ReplayRepositoryIrRemoteScores.cpp \
  src/CMakeLists.txt tests/replay_repository_tests.cpp CMakeLists.txt
git commit -m "feat: persist atomic IR remote score mirrors"
```

---

### Task 5: Plan Receipt Repair Without Guessing

**Files:**
- Create: `src/ir/IrScoreReconciliation.h`
- Create: `src/ir/IrScoreReconciliation.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryRecords.cpp`
- Modify: `tests/replay_repository_tests.cpp`
- Create: `tests/ir_score_reconciliation_tests.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
namespace ir {
struct IrLocalReceiptCandidate {
  int replayId = 0;
  std::string attemptId;
  std::string chartMd5;
  std::string chartSha256;
  int score = 0;
  int lampRank = 0;
  bool eligible = false;
  std::optional<IrSubmissionReceipt> currentReceipt;
  std::optional<std::int64_t> outboxRowId;
  std::optional<IrOutboxState> outboxState;
};

struct IrScoreReconciliationPlan {
  std::vector<IrSubmissionReceipt> upsertedReceipts;
  std::vector<std::int64_t> deletedReceiptIds;
  std::vector<std::int64_t> settledOutboxRowIds;
  std::vector<std::int64_t> purgedSucceededOutboxRowIds;
  int ambiguousReceiptsPreserved = 0;
};

[[nodiscard]] IrScoreReconciliationPlan planScoreReconciliation(
    std::string_view providerId, std::string_view serverOrigin,
    std::span<const IrLocalReceiptCandidate> local,
    std::span<const IrRemoteScore> remote,
    std::int64_t confirmedAtUnixMillis);

struct IrReconciliationReadOutcome {
  enum class Status { Loaded, Invalid, StorageFailure };
  Status status = Status::StorageFailure;
  std::vector<IrLocalReceiptCandidate> candidates;
  std::string diagnostic;
};
}

// ReplayRepository public method:
ir::IrReconciliationReadOutcome LoadIrReconciliationCandidates(
    std::string_view providerId, std::string_view serverOrigin);
```

- [ ] **Step 1: Write the reconciliation matrix**

Prove these rules:

- A current `remoteScoreId` present in the snapshot remains and becomes `observedInSnapshot=true`.
- An eligible local replay without an ID matches only a unique remote `(chart hash, score, lamp)` candidate and receives a snapshot receipt.
- Multiple local attempts with identical proof may link to the same deterministic remote score ID.
- Ineligible, autoplay, course, modified-ruleset, or ambiguous local candidates never gain receipts.
- A matching non-active pending/retry/blocked outbox row is settled as already represented remotely; active rows are impossible during the serialized apply and fail validation if supplied.
- A retained succeeded outbox row is purgeable only after its durable receipt is present in the same plan.
- A snapshot-observed receipt whose remote ID disappeared is deleted.
- A submission receipt never previously observed is preserved and counted as ambiguous when no unambiguous remote match exists, because absence may represent primary-chart/API history changes rather than proof of failed upload.
- Provider/origin boundaries and remote game key mode are enforced. Matching never uses title, artist, timestamp, optional judgements, or LN mode.

Add repository tests that the candidate read happens in one snapshot, includes canonical local score/lamp/hash, eligibility proof inputs, exact origin-scoped receipt, and at most one requested outbox row per attempt. Corrupt replay/provenance rows are skipped with bounded aggregate diagnostics; a storage failure returns no partial candidate vector.

- [ ] **Step 2: Run for RED**

```bash
cmake --build cmake-build-debug --target ir_score_reconciliation_tests \
  replay_repository_tests -j 6
./cmake-build-debug/replay_repository_tests
```

- [ ] **Step 3: Implement deterministic indexed matching**

Index remote scores by ID and by `(sha256-or-md5, score, lamp)`. Reject cross-hash disagreement and ambiguity. Return only mutations; do not call SQLite or networking from the planner.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target ir_score_reconciliation_tests \
  replay_repository_tests -j 6
./cmake-build-debug/ir_score_reconciliation_tests
./cmake-build-debug/replay_repository_tests
git diff --check
git add src/ir/IrScoreReconciliation.* src/ir/CMakeLists.txt \
  src/repositories/ReplayRepository.h \
  src/repositories/ReplayRepositoryRecords.cpp \
  tests/ir_score_reconciliation_tests.cpp tests/replay_repository_tests.cpp \
  CMakeLists.txt
git commit -m "feat: plan conservative IR receipt reconciliation"
```

---

### Task 6: Serialize Manual Sync with Submission Delivery

**Files:**
- Modify: `src/ir/IrSubmissionService.h`
- Modify: `src/ir/IrSubmissionService.cpp`
- Modify: `tests/ir_submission_service_tests.cpp`

**Interfaces:**

```cpp
enum class IrReconciliationPhase {
  Idle,
  Queued,
  Fetching7K,
  Fetching14K,
  Applying,
  Succeeded,
  Failed,
  Cooldown,
};

struct IrReconciliationStatusSnapshot {
  std::uint64_t revision = 0;
  IrReconciliationPhase phase = IrReconciliationPhase::Idle;
  int remoteScores = 0;
  int remoteScoresAdded = 0;
  int remoteScoresRemoved = 0;
  int receiptsUpserted = 0;
  int receiptsDeleted = 0;
  int outboxRowsSettled = 0;
  int ambiguousReceiptsPreserved = 0;
  std::optional<std::chrono::steady_clock::time_point> nextAllowedAt;
  std::string diagnostic;
};

enum class IrReconciliationRequestStatus {
  Accepted,
  AlreadyRunning,
  Cooldown,
  Unsupported,
  ConfigurationRequired,
  ServiceInactive,
};

IrReconciliationRequestStatus
requestUserScoreReconciliation(std::string_view providerId);
IrReconciliationStatusSnapshot
reconciliationStatus(std::string_view providerId) const;
```

- [ ] **Step 1: Write worker/state tests**

With fake time and driver, assert one accepted request transitions through queued, both fetch phases, applying, and succeeded. Assert concurrent taps coalesce, the outbox worker never overlaps HTTP with reconciliation, new outbox work waits without starvation, pause/profile change cancels before apply, and database state is unchanged on cancellation/failure. Assert the cooldown is 60 seconds from completion for both success and failure and uses monotonic time. Confirm a new request works exactly at expiry.

Assert the service loads eligible local candidates and current origin-scoped receipts, calls the pure planner, and invokes `ApplyIrRemoteSnapshot` once. A repository failure publishes `Failed`, never `Succeeded`.

- [ ] **Step 2: Run for RED**

```bash
cmake --build cmake-build-debug --target ir_submission_service_tests -j 6
./cmake-build-debug/ir_submission_service_tests
```

- [ ] **Step 3: Implement one serialized worker command**

Queue reconciliation as a command on the existing service worker rather than creating a second network thread. Copy only normalized provider settings into the command; retrieve the credential at execution time. Publish revisions after every phase change. Drop result application if profile ID, provider origin, or credential generation changed while requests were in flight.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target ir_submission_service_tests -j 6
./cmake-build-debug/ir_submission_service_tests
git diff --check
git add src/ir/IrSubmissionService.* tests/ir_submission_service_tests.cpp
git commit -m "feat: serialize Bokutachi record synchronization"
```

---

### Task 7: Add the Settings Safety Sync

**Files:**
- Modify: `src/ir/IrSettingsPresentation.h`
- Modify: `src/ir/IrSettingsPresentation.cpp`
- Modify: `src/scene/SettingsSceneIr.cpp`
- Modify: `tests/ir_settings_presentation_tests.cpp`
- Modify: `scripts/check_main_menu_settings_anchor.py`

- [ ] **Step 1: Add presentation/action tests**

Require a `showRecordSync`/`canSyncRecords` presentation state only when the driver supports reconciliation, the provider is enabled, a credential exists, and the service is active. Test button copy `Sync IR records`, exact two-request helper text, all progress phases, added/removed record counts, confirmed/removed/ambiguous receipt counts, settled outbox count, bounded failures, cooldown remaining text, and no credential content. Preserve the bottom-anchored Settings button source contract.

- [ ] **Step 2: Run for RED**

```bash
cmake --build cmake-build-debug --target ir_settings_presentation_tests -j 6
./cmake-build-debug/ir_settings_presentation_tests
```

- [ ] **Step 3: Build and refresh the card**

Add the action below the Bokutachi queue controls. It calls only `requestUserScoreReconciliation`; it never performs HTTP in the scene. While the IR settings tab is visible, refresh on reconciliation revision changes. Disable the button during running/cooldown and show the 7K/14K/apply phase plus final imported/receipt counts.

On credential replacement/removal, call `ClearIrAccountEvidence` before committing the key change, then clear cached Bokutachi user IDs. If either durable invalidation fails, keep the old credential. Never implement this with two independently committed delete calls.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target ir_settings_presentation_tests -j 6
./cmake-build-debug/ir_settings_presentation_tests
python3 scripts/check_main_menu_settings_anchor.py
git diff --check
git add src/ir/IrSettingsPresentation.* src/scene/SettingsSceneIr.cpp \
  tests/ir_settings_presentation_tests.cpp scripts/check_main_menu_settings_anchor.py
git commit -m "feat: add Bokutachi record safety sync"
```

---

### Task 8: Project Remote Score and Lamp into Song Select

**Files:**
- Create: `src/ir/IrScoreHistoryProjection.h`
- Create: `src/ir/IrScoreHistoryProjection.cpp`
- Create: `tests/ir_score_history_projection_tests.cpp`
- Modify: `src/repositories/ScoreRepositoryModels.h`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Modify: `src/ir/CMakeLists.txt`
- Modify: `CMakeLists.txt`

**Interfaces:**

```cpp
void projectIrRemoteScores(
    std::span<const IrRemoteScore> remote,
    ScoreClearRankCache &clearRanks,
    ScoreBestCache &bestScores);
```

- [ ] **Step 1: Write projection tests**

Test that the best remote lamp and score merge by SHA256, fall back to MD5 only at the repository lookup boundary, never lower a stronger local best, and populate every LN-mode bucket because remote LN mode is unknown. A remote score wins score comparison by score, then lamp, then achieved/added time using the existing local best policy. Missing combo/BP/gauge/time remains absent; update `ScoreBestSnapshot` only as needed to preserve optionality rather than filling zeros.

Assert remote data does not alter course ranks, ruleset eligibility, local play count, local replay availability, or submission state.

- [ ] **Step 2: Run for RED**

```bash
cmake --build cmake-build-debug --target ir_score_history_projection_tests -j 6
```

- [ ] **Step 3: Implement and integrate projection**

Load the active provider/origin mirror after local score caches load, then project it. When reconciliation status reaches a new successful revision, reload both local caches and the mirror, refresh folder aggregates/chart rows, and preserve selection/scroll. If the remote mirror cannot be read, retain the local caches and surface one bounded diagnostic instead of clearing song-select records.

- [ ] **Step 4: Run full verification**

```bash
cmake --build cmake-build-debug --target ir_score_history_projection_tests \
  replay_repository_tests ir_submission_service_tests \
  ir_settings_presentation_tests tachi_driver_tests -j 6
./cmake-build-debug/ir_score_history_projection_tests
./cmake-build-debug/replay_repository_tests
./cmake-build-debug/ir_submission_service_tests
./cmake-build-debug/ir_settings_presentation_tests
./cmake-build-debug/tachi_driver_tests
ctest --test-dir cmake-build-debug --output-on-failure
cmake --build cmake-build-debug --target main -j 6
git diff --check
```

- [ ] **Step 5: Commit**

```bash
git add src/ir/IrScoreHistoryProjection.* src/ir/CMakeLists.txt \
  src/repositories/ScoreRepositoryModels.h src/scene/MainMenuScene.* \
  tests/ir_score_history_projection_tests.cpp CMakeLists.txt
git commit -m "feat: project imported IR bests into song select"
```

---

## Plan 2 Completion Gate

- One manual action always generates two and only two Tachi requests.
- A failure cannot partially replace the remote mirror or repair/delete receipts.
- Successfully synchronized remote scores improve song-select best score/lamp without claiming local ruleset/LN-mode provenance.
- Settings reports request and mutation progress without exposing secrets.
- Credential replacement/removal cannot leak the previous account's mirror or receipt state.
- Imported rows and partial result recall are intentionally deferred to Plan 3.
