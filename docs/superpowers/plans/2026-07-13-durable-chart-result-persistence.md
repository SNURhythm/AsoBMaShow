# Durable Chart Result Persistence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist every eligible completed chart before its result transition,
with one durable replay-backed outbox, idempotent score projection, crash
recovery, and a truthful user-visible retry state.

**Architecture:** A versioned immutable `ChartResultAttempt` is staged in one
Replay DB transaction with its pending score payload. A coordinator holds one
profile database binding guard while it projects that payload into Score DB by
the same unique attempt ID and acknowledges the outbox. Gameplay invokes the
coordinator before deferred transition work; startup and profile activation
recover retained pending projections.

**Tech Stack:** C++23, SQLite3 guarded migrations and WAL sessions, SDL2,
`std::shared_mutex`, CMake/Ninja, CTest, Bash source audits, Xcode synchronized
groups.

**Design:**
`docs/superpowers/specs/2026-07-13-durable-chart-result-persistence-design.md`

## Global Constraints

- Scope only live, non-practice, non-course, non-replay chart attempts for
  which `resultCapturePolicy()` requires both score and replay persistence.
- Practice, Auto Play, replay playback, and course playback keep their current
  no-persistence behavior.
- The replay plus pending outbox is the authoritative attempt; the score row is
  an idempotent projection.
- New attempt IDs are canonical lower-case version-4 UUIDs. Existing profile
  UUID acceptance remains structurally backward-compatible.
- Legacy score/replay rows keep null attempt IDs and remain repeatable.
- A current payload is never called durable after a staging conflict.
- Failed, malformed, or conflicting pending rows are retained and never
  overwritten or acknowledged.
- One outer `profile_database_activity::WriteGuard` spans every cross-database
  persistence call and recovery batch; no guard is held across user input.
- User messages contain no attempt ID, profile identifier, chart path,
  database filename, SQL text, exception, or replay event data.
- Recovery processes at most 256 oldest pending rows per invocation.
- Add every new implementation file under `src` to desktop/mobile CMake and to
  iOS `membershipExceptions` exactly once.
- Use `scripts/ios_firebase_deploy.sh --build-only` and
  `scripts/android_firebase_deploy.sh --build-only` only; do not upload.

---

### Task 1: Centralize UUIDs and define the immutable attempt model

**Files:**

- Create: `src/Uuid.h`
- Create: `src/Uuid.cpp`
- Create: `src/ResultPersistenceModel.h`
- Create: `src/ResultPersistenceModel.cpp`
- Create: `tests/result_persistence_model_tests.cpp`
- Modify: `src/PlayerProfileManager.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify:
  `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**

- Produces:

```cpp
namespace uuid {
std::string generateV4();
bool isStructurallyValid(std::string_view value) noexcept;
bool isCanonicalLowerV4(std::string_view value) noexcept;
}

namespace result_persistence {
struct ChartScoreWrite {
  std::string chartPath;
  std::string chartMd5;
  std::string chartSha256;
  std::string chartTitle;
  std::string chartArtist;
  int longNoteMode = 0;
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
  float finalGauge = 0.0f;
  int clearType = kClearTypeFailedRank;
  ScoreProvenance provenance = ScoreProvenance::Legacy();
  bool operator==(const ChartScoreWrite &) const = default;
};

struct ChartResultAttempt {
  std::string attemptId;
  ReplayData replay;
  ChartScoreWrite score;
  std::string payloadFingerprint;
};

struct StageReceipt {
  std::string attemptId;
  int replayId = 0;
  std::string createdAt;
  bool scorePending = false;
};

ChartScoreWrite captureChartScoreWrite(
    const bms_parser::ChartMeta &meta, const RhythmState &state,
    const ScoreProvenance &provenance, int storageLongNoteMode);

std::optional<ChartResultAttempt> makeChartResultAttempt(
    std::string attemptId, const bms_parser::ChartMeta &meta,
    const RhythmState &state, const ScoreProvenance &provenance,
    int storageLongNoteMode, ReplayData replay, std::string &diagnostic);
std::string payloadFingerprint(const ReplayData &replay,
                               const ChartScoreWrite &score);
}
```

- `ChartScoreWrite::clearType` is the base gauge rank stored in `scores`.
  `makeChartResultAttempt` compares `ReplayData::clearType` against
  `fullComboRankForPlayback(clearType, fullCombo, provenance.playback)`.
- The fingerprint is `v1` plus a canonical length-prefixed encoding of every
  replay/score field except database row IDs and timestamps.

- [ ] **Step 1: Establish the focused baseline**

Run:

```sh
cmake --build cmake-build-debug --target \
  player_profile_manager_tests replay_db_helper_tests \
  score_provenance_db_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(foundation_profile_manager|replay_db_helper_tests|foundation_provenance_db)$'
```

Expected: all three tests pass and `main` builds. Record commands and results
in `.superpowers/sdd/durable-result-task-1-report.md`.

- [ ] **Step 2: Add model/UUID tests and the test target before production
  files**

Create tests with fixed values, including:

```cpp
constexpr std::string_view kAttemptId =
    "123e4567-e89b-42d3-a456-426614174000";

void testUuidPolicies() {
  expect(uuid::isStructurallyValid(
             "123E4567-E89B-42D3-A456-426614174000"),
         "legacy structural UUID accepts upper case");
  expect(uuid::isCanonicalLowerV4(kAttemptId),
         "attempt UUID accepts canonical lower v4");
  expect(!uuid::isCanonicalLowerV4(
             "123E4567-E89B-42D3-A456-426614174000"),
         "attempt UUID rejects upper case");
  expect(!uuid::isCanonicalLowerV4(
             "123e4567-e89b-12d3-a456-426614174000"),
         "attempt UUID rejects non-v4 version");
}

void testAttemptValidationAndFingerprint() {
  auto fixture = completedAttemptFixture();
  std::string diagnostic;
  auto attempt = result_persistence::makeChartResultAttempt(
      std::string(kAttemptId), fixture.meta, fixture.state,
      fixture.provenance, fixture.storageLongNoteMode, fixture.replay,
      diagnostic);
  expect(attempt.has_value(), "valid completed attempt is accepted");
  expect(attempt && attempt->payloadFingerprint.size() == 64,
         "attempt fingerprint is SHA-256 hex");
  auto changed = fixture.replay;
  changed.events.front().diffMicros += 1;
  expect(attempt && attempt->payloadFingerprint !=
                        result_persistence::payloadFingerprint(
                            changed, attempt->score),
         "event change changes fingerprint");
}
```

Add cases for every vector family, optionals, provenance, float bit patterns,
wrong chart identity, wrong final score/gauge/max combo, valid full-combo
normalization, and invalid attempt ID.

Add `result_persistence_model_tests` to CMake with
`ResultPersistenceModel.cpp`, `Uuid.cpp`, `FileChecksum.cpp`, `ScoreProvenance.cpp`,
`Utils.cpp`, `path.cpp`, and `bms_parser.cpp` plus SDL linkage.

- [ ] **Step 3: Run the new target and capture RED**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target result_persistence_model_tests -j 6
```

Expected: compile fails because `Uuid.h` and `ResultPersistenceModel.h` do not
exist. Record the relevant compiler error.

- [ ] **Step 4: Implement shared UUID behavior and refactor the profile
  manager**

Move the existing random version-4 byte generation into `uuid::generateV4()`.
Implement `isStructurallyValid()` with the profile manager's current
36-character/hyphen/hex policy. Implement `isCanonicalLowerV4()` with lower
hex plus version nibble `4` at index 14 and variant nibble `8..b` at index 19.

Replace private `defaultUuid()` and `isUuid()` use in
`PlayerProfileManager.cpp` with the shared utility without changing profile
acceptance.

- [ ] **Step 5: Implement score capture, normalized validation, and
  fingerprinting**

Build `ChartScoreWrite` once from the state. Accept the storage long-note mode
as an explicit factory argument; production callers must supply
`scoreLongNoteModeForClearLamp(meta)`. Use
`Utils::GetStoragePathUtf8RelativeToDocuments(meta.BmsPath, "BMS/")`,
normalized chart hashes, named judgement counts, the supplied provenance, and
the supplied `storageLongNoteMode` directly. This platform-neutral model must
not include or call `ScoreDBHelper`. Encode every field with explicit sizes and
presence markers into `file_checksum::Sha256`.

Reject invalid attempts with diagnostics that name only the failed invariant;
diagnostics are log-only and must never include paths or input events.

- [ ] **Step 6: Add build metadata and verify GREEN**

Add `Uuid.cpp` and `ResultPersistenceModel.cpp` to `src/CMakeLists.txt` and iOS
`membershipExceptions`. Add `Uuid.cpp` to all three standalone targets that
compile `PlayerProfileManager.cpp`.

Run:

```sh
cmake --build cmake-build-debug --target \
  result_persistence_model_tests player_profile_manager_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(result_persistence_model_tests|foundation_profile_manager)$'
```

Expected: 2/2 pass and `main` builds.

- [ ] **Step 7: Prove mutation sensitivity**

Temporarily make `payloadFingerprint()` omit `ReplayEvent::diffMicros`; rebuild
and run `result_persistence_model_tests`. Expected: the event mutation case
fails. Restore the implementation and rerun to pass.

- [ ] **Step 8: Commit Task 1**

```sh
git add CMakeLists.txt src/CMakeLists.txt src/Uuid.h src/Uuid.cpp \
  src/ResultPersistenceModel.h src/ResultPersistenceModel.cpp \
  src/PlayerProfileManager.cpp tests/result_persistence_model_tests.cpp \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "refactor: centralize result attempt identity"
```

---

### Task 2: Add atomic Replay DB staging and the durable score outbox

**Files:**

- Modify: `src/ReplayDBHelper.h`
- Modify: `src/ReplayDBHelper.cpp`
- Modify: `tests/replay_db_helper_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: `ChartResultAttempt`, `ChartScoreWrite`, and `StageReceipt` from
  Task 1.
- Produces:

```cpp
namespace result_persistence {
enum class StageStatus {
  Staged,
  AlreadyStaged,
  StorageFailure,
  IntegrityConflict,
};
struct StageOutcome {
  StageStatus status = StageStatus::StorageFailure;
  std::optional<StageReceipt> receipt;
  std::string diagnostic;
};

struct PendingChartScoreWrite {
  std::string attemptId;
  int replayId = 0;
  std::string createdAt;
  ChartScoreWrite score;
};
enum class PendingReadStatus {
  Found,
  NotFound,
  StorageFailure,
  IntegrityConflict,
};
struct PendingReadOutcome {
  PendingReadStatus status = PendingReadStatus::StorageFailure;
  std::optional<PendingChartScoreWrite> value;
  std::string diagnostic;
};
struct PendingBatchEntry {
  PendingReadStatus status = PendingReadStatus::IntegrityConflict;
  std::string attemptId;
  std::optional<PendingChartScoreWrite> value;
  std::string diagnostic;
};
struct PendingBatchOutcome {
  bool storageAvailable = false;
  std::vector<PendingBatchEntry> entries;
  std::string diagnostic;
};

enum class RecoveryAttemptKind { StorageFailure, IntegrityConflict };
enum class RecoveryMarkStatus { Recorded, NotFound, StorageFailure };
struct RecoveryMarkOutcome {
  RecoveryMarkStatus status = RecoveryMarkStatus::StorageFailure;
  std::string diagnostic;
};

enum class AcknowledgeStatus {
  Acknowledged,
  AlreadyAcknowledged,
  StorageFailure,
  IntegrityConflict,
};
struct AcknowledgeOutcome {
  AcknowledgeStatus status = AcknowledgeStatus::StorageFailure;
  std::string diagnostic;
};
}

result_persistence::StageOutcome
ReplayDBHelper::StageChartResult(
    const result_persistence::ChartResultAttempt &attempt);
result_persistence::PendingReadOutcome
ReplayDBHelper::LoadPendingChartScore(std::string_view attemptId);
result_persistence::PendingBatchOutcome
ReplayDBHelper::ListPendingChartScores(std::size_t limit = 256);
result_persistence::AcknowledgeOutcome
ReplayDBHelper::AcknowledgePendingChartScore(std::string_view attemptId,
                                             int replayId);
result_persistence::RecoveryMarkOutcome
ReplayDBHelper::RecordPendingChartScoreRecoveryAttempt(
    std::string_view attemptId,
    result_persistence::RecoveryAttemptKind kind);
```

- [ ] **Step 1: Add migration and staging RED tests**

Extend `replay_db_helper_tests.cpp` with:

```cpp
void testVersion4MigrationAddsResultOutbox();
void testLegacyReplayRowsRemainRepeatableWithNullAttemptId();
void testStageChartResultIsAtomicAndReturnsTimestamp();
void testIdenticalAttemptIsIdempotent();
void testChangedPayloadForSameAttemptConflicts();
void testAcknowledgedAttemptRemainsIdempotentByFingerprint();
void testOutboxInsertFailureRollsBackReplayAndChildren();
void testPendingReadsDistinguishMissingFailureAndConflict();
void testRecoverySnapshotKeepsMalformedRowsAndContinues();
void testRecoverySnapshotPrioritizesNeverAttemptedRows();
void testFutureVersionFivePlusOneIsRejected();
```

Create a version-4 fixture explicitly, open it through `ReplayDBHelper`, and
inspect `PRAGMA user_version`, `PRAGMA table_info(replays)`, and the unique
partial index. For rollback, install:

```sql
CREATE TRIGGER fail_pending_score
BEFORE INSERT ON pending_chart_score_writes
BEGIN
  SELECT RAISE(ABORT, 'forced pending failure');
END;
```

Assert that both the attempted replay row and every child row remain absent.

- [ ] **Step 2: Run RED against schema version 4**

Run:

```sh
cmake --build cmake-build-debug --target replay_db_helper_tests -j 6
ctest --test-dir cmake-build-debug -R '^replay_db_helper_tests$' \
  --output-on-failure
```

Expected: compile fails on the missing staging APIs or the new version-4
migration assertion fails because current schema version is 4.

- [ ] **Step 3: Implement Replay DB version 5 migration**

Set `ReplayDBHelper::kCurrentSchemaVersion = 5`. Add one migration transaction
that:

```sql
ALTER TABLE replays ADD COLUMN attempt_id TEXT;
ALTER TABLE replays ADD COLUMN attempt_fingerprint TEXT;
CREATE UNIQUE INDEX idx_replays_attempt_id
  ON replays(attempt_id) WHERE attempt_id IS NOT NULL;
CREATE TABLE pending_chart_score_writes (
  attempt_id TEXT PRIMARY KEY,
  replay_id INTEGER NOT NULL UNIQUE,
  chart_path TEXT NOT NULL,
  chart_md5 TEXT NOT NULL,
  chart_sha256 TEXT NOT NULL,
  chart_title TEXT NOT NULL,
  chart_artist TEXT NOT NULL,
  ln_mode INTEGER NOT NULL,
  score INTEGER NOT NULL,
  max_score INTEGER NOT NULL,
  max_combo INTEGER NOT NULL,
  combo_break INTEGER NOT NULL,
  pgreat INTEGER NOT NULL,
  great INTEGER NOT NULL,
  good INTEGER NOT NULL,
  bad INTEGER NOT NULL,
  poor INTEGER NOT NULL,
  kpoor INTEGER NOT NULL,
  fast INTEGER NOT NULL,
  slow INTEGER NOT NULL,
  final_gauge REAL NOT NULL,
  clear_type INTEGER NOT NULL,
  ruleset_version INTEGER NOT NULL,
  eligibility INTEGER NOT NULL,
  provenance_json TEXT NOT NULL,
  created_at TEXT NOT NULL,
  recovery_attempts INTEGER NOT NULL DEFAULT 0,
  last_recovery_at TEXT,
  FOREIGN KEY(replay_id) REFERENCES replays(id) ON DELETE CASCADE
);
CREATE INDEX idx_pending_chart_score_created
  ON pending_chart_score_writes(
    recovery_attempts, last_recovery_at, created_at, attempt_id);
PRAGMA user_version = 5;
```

Use the repository's migration helpers rather than executing the block as one
raw string. Preserve future-version rejection and the existing version-2 to
version-4 steps.

- [ ] **Step 4: Refactor replay row insertion for optional attempt metadata**

Extend only the internal insert routine with nullable attempt ID/fingerprint
bindings. Keep `SaveReplay()` passing null so legacy callers remain unchanged.
Do not open a nested transaction from the internal routine used by staging.

- [ ] **Step 5: Implement staging, typed reads, and acknowledgement**

For a new attempt, use one `SqliteTransactionHandle` around replay children,
timestamp lookup, and pending-row insertion. For an existing attempt ID, compare
the stored fingerprint first and return `AlreadyStaged` with the existing
receipt only on equality.

Decode every pending field and provenance strictly. A malformed row becomes an
`IntegrityConflict` entry and remains present. `AcknowledgePendingChartScore`
deletes only the matching `(attempt_id, replay_id)` pair; zero deleted rows are
`AlreadyAcknowledged` only if the matching replay/fingerprint still exists and
no pending row does.

Select recovery rows with never-attempted rows first, then the oldest retry
marker:

```sql
ORDER BY recovery_attempts,
         last_recovery_at,
         created_at,
         attempt_id
LIMIT ?
```

`RecordPendingChartScoreRecoveryAttempt` updates only retry metadata for the
named retained row:

```sql
UPDATE pending_chart_score_writes
SET recovery_attempts = recovery_attempts + 1,
    last_recovery_at = CURRENT_TIMESTAMP
WHERE attempt_id = ?;
```

Because `ReplayDBHelper.cpp` now calls the model/UUID implementations, add
`ResultPersistenceModel.cpp` and `Uuid.cpp` to every standalone target that
compiles it: `replay_db_helper_tests`, `player_profile_manager_tests`,
`profile_switch_tests`, and `profile_archive_tests`.

- [ ] **Step 6: Run focused GREEN and mutation checks**

Run:

```sh
cmake --build cmake-build-debug --target replay_db_helper_tests -j 6
ctest --test-dir cmake-build-debug -R '^replay_db_helper_tests$' \
  --output-on-failure
```

Expected: 1/1 passes.

Then temporarily commit the replay before inserting the pending score. Expected:
`testOutboxInsertFailureRollsBackReplayAndChildren` fails. Restore and rerun.

- [ ] **Step 7: Commit Task 2**

```sh
git add CMakeLists.txt src/ReplayDBHelper.h src/ReplayDBHelper.cpp \
  tests/replay_db_helper_tests.cpp
git commit -m "feat: stage chart results in replay outbox"
```

---

### Task 3: Add idempotent Score DB projection and exact previous-best exclusion

**Files:**

- Modify: `src/ScoreDBHelper.h`
- Modify: `src/ScoreDBHelper.cpp`
- Modify: `tests/score_provenance_db_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: `PendingChartScoreWrite` from Task 2.
- Produces:

```cpp
namespace result_persistence {
enum class ProjectionStatus {
  Inserted,
  AlreadyPresent,
  StorageFailure,
  IntegrityConflict,
};
struct ProjectionOutcome {
  ProjectionStatus status = ProjectionStatus::StorageFailure;
  std::string diagnostic;
};
}

result_persistence::ProjectionOutcome
ScoreDBHelper::SaveProjectedScore(
    const result_persistence::PendingChartScoreWrite &pending);

std::optional<ScoreBestSnapshot> ScoreDBHelper::LoadBestScore(
    const bms_parser::ChartMeta &chartMeta,
    const std::optional<std::string> &beforeCreatedAt = std::nullopt,
    const std::optional<std::string> &excludeAttemptId = std::nullopt);
```

- [ ] **Step 1: Add Score DB migration/projection RED cases**

Add:

```cpp
void testVersion8MigrationAddsAttemptIdentity();
void testLegacyNullAttemptScoresRemainRepeatable();
void testProjectedScoreIsIdempotent();
void testProjectedScoreConflictDoesNotMutateExistingRow();
void testProjectedScoreUsesReplayTimestamp();
void testProjectedRetryUpdatesSummaryCachesOnce();
void testPreviousBestExcludesExactAttemptAtSameTimestamp();
void testFutureVersionNinePlusOneIsRejected();
```

The idempotency case calls `SaveProjectedScore()` twice with the same payload,
expects `Inserted` then `AlreadyPresent`, and asserts one score row. The conflict
case changes one judgement count under the same ID and asserts
`IntegrityConflict`, unchanged row count, and unchanged original values.

- [ ] **Step 2: Run RED against schema version 8**

Run:

```sh
cmake --build cmake-build-debug --target score_provenance_db_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_provenance_db$' \
  --output-on-failure
```

Expected: compile fails on `SaveProjectedScore` or the migration assertion
reports version 8.

- [ ] **Step 3: Implement Score DB version 9 migration**

Add nullable `attempt_id` and a unique partial index inside one guarded
migration transaction:

```sql
ALTER TABLE scores ADD COLUMN attempt_id TEXT;
CREATE UNIQUE INDEX idx_scores_attempt_id
  ON scores(attempt_id) WHERE attempt_id IS NOT NULL;
PRAGMA user_version = 9;
```

Update every trusted-version check and future-version test to 9.

- [ ] **Step 4: Refactor the score binder around `ChartScoreWrite`**

Make the existing runtime/compatibility `SaveScore(meta, state, provenance)`
construct `ChartScoreWrite` and delegate to one internal binder with null
attempt ID and default timestamp. `SaveProjectedScore()` supplies a non-null ID
and explicit timestamp.

Add `ResultPersistenceModel.cpp` and `Uuid.cpp` to
`score_provenance_db_tests`; the profile-oriented targets already receive them
in Task 2 because they compile both helpers.

On `SQLITE_CONSTRAINT_UNIQUE`, query by attempt ID and compare every immutable
field, the shared `created_at`, and canonical provenance. Return
`AlreadyPresent` only on exact equality; return `IntegrityConflict` otherwise.
Do not use broad `INSERT OR IGNORE`.

- [ ] **Step 5: Add exact attempt exclusion to best-score queries**

When `excludeAttemptId` is present, add:

```sql
AND (attempt_id IS NULL OR attempt_id <> ?)
```

to the candidate query before ordering. Keep `beforeCreatedAt` behavior for
legacy replay playback. Bind both filters deterministically when both exist.

- [ ] **Step 6: Run GREEN and prove conflict mutation sensitivity**

Run:

```sh
cmake --build cmake-build-debug --target score_provenance_db_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_provenance_db$' \
  --output-on-failure
```

Expected: 1/1 passes.

Temporarily return `AlreadyPresent` for every unique conflict. Expected:
`testProjectedScoreConflictDoesNotMutateExistingRow` fails. Restore and rerun.

- [ ] **Step 7: Commit Task 3**

```sh
git add CMakeLists.txt src/ScoreDBHelper.h src/ScoreDBHelper.cpp \
  tests/score_provenance_db_tests.cpp
git commit -m "feat: make score projection idempotent"
```

---

### Task 4: Add the guarded cross-database coordinator and recovery

**Files:**

- Create: `src/ResultPersistenceCoordinator.h`
- Create: `src/ResultPersistenceCoordinator.cpp`
- Create: `tests/result_persistence_coordinator_tests.cpp`
- Create: `tests/result_persistence_integration_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify:
  `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**

- Consumes: all typed Replay DB and Score DB outcomes from Tasks 2 and 3.
- Produces:

```cpp
namespace result_persistence {
enum class SaveState {
  Saved,
  Unstaged,
  PendingScore,
  PendingAcknowledgement,
  UnstagedConflict,
  PendingConflict,
};
struct SaveOutcome {
  SaveState state = SaveState::Unstaged;
  std::optional<StageReceipt> receipt;
  std::string userMessage;
  std::string diagnostic;
  [[nodiscard]] bool saved() const noexcept {
    return state == SaveState::Saved;
  }
  [[nodiscard]] bool durable() const noexcept;
};
struct RecoverySummary {
  std::size_t attempted = 0;
  std::size_t saved = 0;
  std::size_t pending = 0;
  std::size_t conflicts = 0;
  std::string userMessage;
  std::string diagnostic;
};

struct Dependencies {
  std::function<StageOutcome(const ChartResultAttempt &)> stage;
  std::function<PendingReadOutcome(std::string_view)> loadPending;
  std::function<PendingBatchOutcome(std::size_t)> listPending;
  std::function<ProjectionOutcome(const PendingChartScoreWrite &)> project;
  std::function<AcknowledgeOutcome(std::string_view, int)> acknowledge;
  std::function<RecoveryMarkOutcome(std::string_view, RecoveryAttemptKind)>
      recordRecoveryAttempt;
};

class Coordinator {
public:
  Coordinator(ScoreDBHelper &score, ReplayDBHelper &replay);
  explicit Coordinator(Dependencies dependencies);
  SaveOutcome persist(const ChartResultAttempt &attempt);
  RecoverySummary recoverAll(std::size_t limit = 256);
};
}
```

- [ ] **Step 1: Write pure state-machine RED tests**

Use event-recording fakes for these exact sequences:

```cpp
void testPersistOrdersStageLoadProjectAcknowledge();
void testStageStorageFailureReturnsTruthfulUnstagedMessage();
void testStageConflictReturnsUnstagedConflictWithoutReceipt();
void testProjectionFailureRetainsPendingScore();
void testProjectionConflictReturnsDurablePendingConflict();
void testAcknowledgeFailureIsDurableAndRetryable();
void testAcknowledgeConflictRetainsPendingConflict();
void testRetrySkipsAlreadyConfirmedReplayAndScore();
void testRecoveryContinuesAfterMalformedAndFailedRows();
void testMessagesNeverContainInjectedDiagnostics();
void testRecoveryLimitIsExactly256();
void testPersistentFirstBatchDoesNotStarveNewValidRow();
```

For every case assert callback count and exact ordered event list, not only the
final state.

- [ ] **Step 2: Add real-helper integration RED tests**

Use temporary replay/score databases and one fixed attempt:

```cpp
void testPersistCreatesOneReplayOneScoreNoPending();
void testCrashAfterStageRecoversExactlyOneScore();
void testCommittedScoreBeforeAckRecoversWithoutDuplicate();
void testProfileSwitchCannotAcquireGateMidPersist();
void testRecoveryRetainsConflictAndProcessesLaterValidRow();
```

In the gate case, pause inside the projection fake and have another thread
construct `profile_database_activity::SwitchGuard`; assert it does not own the
lock until `persist()` returns.

- [ ] **Step 3: Run the new targets and capture RED**

Run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target \
  result_persistence_coordinator_tests \
  result_persistence_integration_tests -j 6
```

Expected: compile fails because `ResultPersistenceCoordinator.h` is absent.

- [ ] **Step 4: Implement pure state mapping and exact messages**

Use the exact messages from the design. Map a staging conflict only to
`UnstagedConflict` and never attach the existing receipt to the current
payload. Map projection/ack conflicts to `PendingConflict` and retain the
current receipt. Never concatenate `diagnostic` into `userMessage`.

- [ ] **Step 5: Hold one binding guard around each operation**

The first executable statement in `persist()` and `recoverAll()` is:

```cpp
profile_database_activity::WriteGuard bindingLease;
```

Keep it alive through all helper callbacks and release it before returning.
Do not store it as a class member. The real constructor binds helper adapters;
the injected constructor binds fakes for unit tests.

- [ ] **Step 6: Implement bounded independent recovery**

Call `listPending(std::min(limit, std::size_t{256}))` once. For each entry,
count malformed rows as conflicts and continue. For valid rows, project and
acknowledge independently. Before moving past any retained failure or conflict,
call `recordRecoveryAttempt` with the typed reason. A query-level storage
failure returns one pending aggregate warning without deleting anything.

The fairness integration fixture inserts 256 permanent conflicts followed by
one valid pending row. First recovery marks the conflict batch. The second
recovery must select and save the never-attempted valid row, leaving exactly the
256 conflicts retained.

- [ ] **Step 7: Add build metadata and run GREEN**

Add the coordinator implementation to main CMake and iOS membership once. Add
both test targets and CTest registrations.

Run:

```sh
cmake --build cmake-build-debug --target \
  result_persistence_coordinator_tests \
  result_persistence_integration_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(result_persistence_coordinator_tests|result_persistence_integration_tests)$'
```

Expected: 2/2 pass and `main` builds.

- [ ] **Step 8: Prove binding and acknowledgement mutations**

First move `WriteGuard` below staging. Expected: the gate integration test
fails. Restore. Then delete acknowledgement from the success path. Expected:
the one-replay/one-score integration test fails because pending count is one.
Restore and rerun 2/2.

- [ ] **Step 9: Commit Task 4**

```sh
git add CMakeLists.txt src/CMakeLists.txt \
  src/ResultPersistenceCoordinator.h src/ResultPersistenceCoordinator.cpp \
  tests/result_persistence_coordinator_tests.cpp \
  tests/result_persistence_integration_tests.cpp \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat: coordinate durable result persistence"
```

---

### Task 5: Move live chart persistence before transition and add truthful UI

**Files:**

- Create: `scripts/check_result_persistence_flow.sh`
- Modify: `src/context.h`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/skin/DefaultSkin.cpp` only if a named status host is needed
- Modify: `CMakeLists.txt`

**Interfaces:**

- `ApplicationContext` owns one
  `result_persistence::Coordinator resultPersistence` bound to the active
  singleton helpers.
- `GamePlayScene` retains one immutable attempt and one `SaveOutcome`:

```cpp
struct ResultPersistenceOptions {
  std::shared_ptr<const result_persistence::ChartResultAttempt> attempt;
  result_persistence::SaveOutcome outcome;
};
```

- `ResultScene` receives `ResultPersistenceOptions`, renders it, and retries by
  calling `context.resultPersistence.persist(*attempt)`.

- [ ] **Step 1: Write the structural audit and prove current RED**

Create an executable script that fails unless all conditions hold:

- the eligible branch calls `makeChartResultAttempt` and
  `context.resultPersistence.persist` before the `defer` inside
  `scheduleResultTransition`;
- a non-saved outcome forces delay to zero;
- `ResultScene.cpp` contains no direct `SaveScore(`, `SaveReplay(`,
  `SaveCourseScore(`, or `SaveCourseReplay(` call for the chart path;
- `ResultScene` has exact `Retry Save` and `Continue Without Saving` actions;
- `exitResult()` refuses to leave while an unresolved result has not received
  an explicit continue decision;
- previous-best loading passes `excludeAttemptId` for a staged live result;
- practice/replay/Auto Play/course policy branches do not stage.

Run:

```sh
chmod +x scripts/check_result_persistence_flow.sh
scripts/check_result_persistence_flow.sh
```

Expected: FAIL because current persistence occurs only in `ResultScene`.

- [ ] **Step 2: Add the coordinator and attempt state to runtime objects**

Construct `ApplicationContext::resultPersistence` after helper references are
available. In `GamePlayScene`, create the attempt once after
`finishReplayRecording()` and only when both capture-policy persistence flags
are true. Reuse the same ID/outcome if scheduling is requested twice.

- [ ] **Step 3: Persist before installing deferred transition work**

Inside `scheduleResultTransition`, before `defer(...)`, create the attempt with
`scoreLongNoteModeForClearLamp(chart->Meta)` and then persist it:

```cpp
if (capturePolicy.persistScore && capturePolicy.persistReplay) {
  ensureResultPersistenceAttempt();
  resultPersistenceOptions.outcome =
      context.resultPersistence.persist(*resultPersistenceOptions.attempt);
  if (!resultPersistenceOptions.outcome.saved()) {
    delayMillis = 0;
  }
}
```

Log only the typed state and diagnostic. Do not log the replay payload or path.
Pass the attempt/outcome by value/shared immutable pointer into `ResultScene`.
When an outcome has a receipt, copy its replay row ID and captured timestamp
into the mutable presentation/retry `ReplayData`; IDs and timestamps are
excluded from the immutable payload fingerprint. Do the same after a successful
Retry Save in `ResultScene`, so replay playback launched from the current result
keeps the correct previous-best timestamp boundary.

- [ ] **Step 4: Remove duplicate ResultScene save ownership**

Delete `scoreSaved`, `replaySaved`, `saveScore()`, and `saveReplay()` from the
chart result path. Keep course persistence unchanged because it is outside this
spec. Rename any remaining `replayToSave` use to presentation/analytics intent
if needed so it no longer implies storage ownership.

Load previous best with the staged attempt ID. If staging never succeeded,
leave `excludeAttemptId` empty because the current score does not exist.

- [ ] **Step 5: Add a blocking in-scene persistence decision panel**

Before normal result actions, create a named status host containing:

- exact coordinator message;
- `Retry Save`, calling the same coordinator with the same attempt;
- `Continue Without Saving`, recording an in-memory explicit decision only.

While unresolved, hide/disable normal result actions and have `exitResult()`
return without leaving. On `Saved`, hide the panel and enable actions. On
explicit continue, retain any durable outbox row, hide the panel, and enable
actions. `Unstaged` and `UnstagedConflict` copy must state that continuing
discards the current result; pending states state that automatic recovery will
retry.

- [ ] **Step 6: Run audit GREEN, focused tests, and main build**

Run:

```sh
scripts/check_result_persistence_flow.sh
cmake --build cmake-build-debug --target \
  result_persistence_model_tests \
  result_persistence_coordinator_tests \
  result_persistence_integration_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^result_persistence_(model|coordinator|integration)_tests$'
```

Expected: audit passes, 3/3 tests pass, and `main` builds.

- [ ] **Step 7: Prove order and UI mutations**

Temporarily move persistence below `defer`; audit must fail. Restore. Remove the
`exitResult()` unresolved guard; audit must fail. Restore. Replace the
unstaged-conflict copy with the durable-conflict copy; the exact-message unit
test must fail. Restore and rerun all focused checks.

- [ ] **Step 8: Commit Task 5**

```sh
git add CMakeLists.txt scripts/check_result_persistence_flow.sh src/context.h \
  src/scene/play/GamePlayScene.h src/scene/play/GamePlayScene.cpp \
  src/scene/ResultScene.h src/scene/ResultScene.cpp src/skin/DefaultSkin.cpp
git commit -m "fix: persist chart results before transition"
```

---

### Task 6: Recover pending projections at startup and profile activation

**Files:**

- Create: `src/ApplicationResultRecovery.h`
- Create: `src/ApplicationResultRecovery.cpp`
- Create: `tests/application_result_recovery_tests.cpp`
- Modify: `src/main.cpp`
- Modify: `src/ProfileSessionCoordinator.h`
- Modify: `src/ProfileSessionCoordinator.cpp`
- Modify: `src/context.h`
- Modify: `src/scene/ProfileSettingsController.cpp` only if existing success
  warning behavior needs no-op clarification
- Modify: `tests/profile_switch_tests.cpp`
- Modify: `scripts/check_result_persistence_flow.sh`
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify:
  `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**

- Add to `ProfileSessionDependencies`:

```cpp
std::function<result_persistence::RecoverySummary()> recoverPendingResults;
```

- A successful switch with `summary.pending > 0` or conflicts returns success
  with `summary.userMessage` in `ProfileSwitchResult::message`; the existing
  controller renders a non-empty success message as a warning.

- Produces testable startup orchestration:

```cpp
namespace application_result_recovery {
struct Dependencies {
  std::function<result_persistence::RecoverySummary()> recover;
  std::function<void(const result_persistence::RecoverySummary &)>
      reportWarning;
  std::function<void()> runReadyRuntime;
};
void execute(const Dependencies &dependencies);
}
```

`execute` calls recovery exactly once, reports exactly once only when
`userMessage` is non-empty, and then calls the ready runtime exactly once. Its
callbacks are mandatory and non-throwing; production adapters contain
operational failures in typed summaries.

- [ ] **Step 1: Add startup/profile recovery RED tests**

Extend profile-switch fixtures with an injected recovery callback and add:

```cpp
void testTargetRecoveryRunsAfterBothDatabaseBindsBeforeCacheRefresh();
void testRecoveryWarningDoesNotRollbackSuccessfulSwitch();
void testRecoveryExceptionBecomesSanitizedWarning();
void testRollbackRestoresOldBindingsAfterLaterFailure();
void testRecoveryRunsUnderExistingSwitchGuardWithoutDeadlock();
```

Record an event list and require:

```text
bind-score, bind-replay, recover-results, apply-input, refresh-caches, commit
```

or amend the expected order only if existing rollback safety requires recovery
after commit; in either case recovery must see both target bindings and cache
publication must follow successful projections.

Create `application_result_recovery_tests.cpp` with exact order assertions:

```cpp
void testCleanRecoveryRunsRuntimeWithoutWarning();
void testPendingRecoveryWarnsBeforeRuntime();
void testConflictRecoveryWarnsBeforeRuntime();
```

For warning cases require the event vector to equal
`{"recover", "warning", "runtime"}`. For the clean case require
`{"recover", "runtime"}`. Pass a warning summary and prove runtime still runs.

- [ ] **Step 2: Add the test target, extend the source audit, and capture RED**

Require:

- startup recovery after database readiness and before `SceneManager` creation;
- `main.cpp` delegates that order to `application_result_recovery::execute` and
  passes scene registration only as `runReadyRuntime`;
- one sanitized SDL warning when the recovery summary has pending/conflicts;
- profile recovery after both binds and before cache refresh;
- no attempt IDs or diagnostics passed to the warning reporter.

Add `application_result_recovery_tests` with the new test and implementation
source and register it with CTest. Before creating the production files, run:

```sh
cmake -S . -B cmake-build-debug
cmake --build cmake-build-debug --target \
  application_result_recovery_tests -j 6
scripts/check_result_persistence_flow.sh
```

Expected: compile fails because `ApplicationResultRecovery.h` is absent, and
the audit fails on missing startup/profile recovery wiring.

- [ ] **Step 3: Implement profile activation recovery**

Invoke the injected callback while the existing `SwitchGuard` is held and both
target helpers are bound. Catch exceptions and convert them to the exact
aggregate warning from the design. Do not roll back a valid switch solely due
to retained pending projections. Preserve a recovery warning in the successful
result message; do not add another warning field.

- [ ] **Step 4: Implement startup recovery and warning adapter**

Split the existing ready body so scene registration lives only in
`runReadyApplicationAfterResultRecovery(context)`. The application-startup
ready callback invokes:

```cpp
application_result_recovery::execute({
    .recover = [&] { return context.resultPersistence.recoverAll(); },
    .reportWarning = [&](const auto &recovery) {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING,
                               "AsoBMaShow Result Recovery",
                               recovery.userMessage.c_str(), window);
    },
    .runReadyRuntime = [&] {
      runReadyApplicationAfterResultRecovery(context);
    },
});
```

Log aggregate counts and technical diagnostics separately. A dialog failure is
logged but does not delete pending rows or make startup fatal.

Add `ApplicationResultRecovery.cpp` to main CMake and iOS
`membershipExceptions` exactly once.

- [ ] **Step 5: Run focused GREEN and mutations**

Run:

```sh
scripts/check_result_persistence_flow.sh
cmake --build cmake-build-debug --target \
  application_startup_tests application_result_recovery_tests \
  profile_switch_tests \
  result_persistence_integration_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(application_startup_tests|application_result_recovery_tests|foundation_profile_switch|result_persistence_integration_tests)$'
```

Expected: audit passes, 4/4 tests pass, and `main` builds.

Temporarily move profile recovery before replay bind; the order test and audit
must fail. Restore. Temporarily omit startup recovery; audit must fail. Restore.

- [ ] **Step 6: Commit Task 6**

```sh
git add CMakeLists.txt src/CMakeLists.txt src/main.cpp src/context.h \
  src/ApplicationResultRecovery.h src/ApplicationResultRecovery.cpp \
  src/ProfileSessionCoordinator.h src/ProfileSessionCoordinator.cpp \
  src/scene/ProfileSettingsController.cpp \
  tests/profile_switch_tests.cpp tests/application_result_recovery_tests.cpp \
  scripts/check_result_persistence_flow.sh \
  ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "fix: recover pending chart results"
```

---

### Task 7: Final verification, scope audit, and independent review

**Files:**

- Modify: `.superpowers/sdd/progress.md` (ignored execution ledger only)
- No production changes unless a reviewer identifies a confirmed defect.

- [ ] **Step 1: Run all affected builds/tests from a fresh command**

Run:

```sh
cmake --build cmake-build-debug --target \
  result_persistence_model_tests replay_db_helper_tests \
  score_provenance_db_tests result_persistence_coordinator_tests \
  result_persistence_integration_tests application_startup_tests \
  application_result_recovery_tests \
  profile_switch_tests player_profile_manager_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure \
  -R '^(result_persistence_.*|replay_db_helper_tests|foundation_provenance_db|application_startup_tests|foundation_profile_switch|foundation_profile_manager)$'
```

Expected: every affected test passes.

- [ ] **Step 2: Run the full desktop suite and structural checks**

Run:

```sh
ctest --test-dir cmake-build-debug --output-on-failure
scripts/check_result_persistence_flow.sh
git diff --check 3e4f614..HEAD
```

Use the actual final design commit if it changed after plan review. Expected:
full CTest passes, audit passes, and diff check is clean.

- [ ] **Step 3: Run mobile build-only verification**

Run:

```sh
scripts/ios_firebase_deploy.sh --build-only
scripts/android_firebase_deploy.sh --build-only
```

Expected: iOS `BUILD SUCCEEDED` and Android `BUILD SUCCESSFUL`; no upload.

- [ ] **Step 4: Verify exact scope and generated-parser exclusion**

Run:

```sh
git status --short
git diff --name-only 3e4f614..HEAD
git diff --name-only 3e4f614..HEAD -- src/bms_parser.hpp src/bms_parser.cpp
```

Expected: worktree clean, only plan-owned persistence/build/test files changed,
and no parser file changed. Substitute the actual design commit hash after any
spec amendment.

- [ ] **Step 5: Request independent quality and acceptance reviews**

Create a review package for the design-to-HEAD range. Require reviewers to
verify:

- schema migration/future-version safety;
- atomic replay+outbox staging;
- same-ID idempotency and conflict comparison;
- outer profile binding guard lifetime;
- delayed transition ordering;
- truthful durable versus unstaged conflict UI;
- previous-best exact exclusion;
- startup/profile recovery retention and privacy;
- legacy behavior and cross-platform build metadata.

Address every Critical/Important finding, rerun affected and full verification,
and obtain READY from both reviewers.

- [ ] **Step 6: Close the execution ledger**

Record each task commit, RED/GREEN/mutation evidence, full test count, mobile
build-only results, audit result, scope result, and final review verdict in
`.superpowers/sdd/progress.md`.
