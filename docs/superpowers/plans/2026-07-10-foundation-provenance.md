# Ruleset and Score Provenance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Snapshot an immutable, versioned ruleset/provenance document once per attempt and persist the same document with chart/course scores and replays.

**Architecture:** A deterministic value object captures ruleset, effective judge windows, chart randomization, gauge/options/assist, device categories, and eligibility. GamePlayScene creates it after effective course constraints are known; ResultScene passes it unchanged to independently migrated score and replay stores.

**Tech Stack:** C++23, nlohmann/json, SQLite `PRAGMA user_version`, existing ScoreDBHelper/ReplayDBHelper, CMake/CTest.

## Global Constraints

- Work only on task branches/worktrees from `feature/player-foundations`; never edit or commit on `develop`.
- Existing outcome values and row counts must remain unchanged during migration.
- Score DB migrates `4 → 5`; replay DB migrates `2 → 3`; a future `user_version` fails before any DDL.
- Ruleset version `1` names the behavior immediately before this milestone; version `0` is legacy only.
- Physical device IDs are forbidden. Provenance records only device categories.
- Practice/autoplay/constraint or rule override is `Modified`; migrated records are `LegacyUnverified`; only an unmodified standard ruleset-1 play is `Verified`.
- New sources must be registered with CTest and the iOS membership exception list.

---

### Task 1: Define deterministic provenance values

**Files:**
- Create: `src/AssistOptionUtils.h`
- Create: `src/ScoreProvenance.h`
- Create: `src/ScoreProvenance.cpp`
- Modify: `src/ReplayData.h`
- Create: `tests/score_provenance_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Consumes: chart metadata, effective `Judge::timingWindows`, current gauge/option/assist types.
- Produces: `RulesetDescriptor`, `ScoreProvenance`, deterministic JSON, attempt builder, and course merge.

- [ ] **Step 1: Write failing contract/serialization tests**

```cpp
const RulesetDescriptor rules = RulesetDescriptor::Current();
assert(rules.version == 1);
assert(rules.scoringModel == "asobmashow-v1");
ScoreProvenance value = sampleVerifiedProvenance();
const std::string first = serializeScoreProvenance(value);
const std::string second = serializeScoreProvenance(value);
assert(first == second);
std::string error;
assert(deserializeScoreProvenance(first, error) == value);
```

Add exact cases for verified/modified/legacy classification, signed early/late microseconds, future schema rejection, ordered course stage hashes, and course merge taking the least eligible state (`LegacyUnverified` worse than `Modified`, which is worse than `Verified`).

- [ ] **Step 2: Register and verify red**

Create `score_provenance_tests`, register `foundation_provenance_contract`, and confirm missing-header compilation failure.

- [ ] **Step 3: Implement exact public types**

First move the existing `assist_options` namespace unchanged from `ReplayData.h` into `AssistOptionUtils.h`, then include that focused header from both `ReplayData.h` and `ScoreProvenance.h`. This prevents a `ReplayData`/`ScoreProvenance` include cycle.

```cpp
enum class ScoreEligibility : int { Verified=0, Modified=1, LegacyUnverified=2 };
enum class JudgeRankSource : int { Chart=0, CourseConstraint=1, Override=2, Unknown=3 };
enum class InputDeviceCategory : int { Keyboard=0, GameController=1, Joystick=2, Touch=3, Midi=4, Unknown=5 };

struct RulesetDescriptor {
  static constexpr int kCurrentVersion = 1;
  int version = kCurrentVersion;
  std::string scoringModel = "asobmashow-v1";
  std::string judgementModel = "bms-rank-v1";
  std::string gaugeModel = "asobmashow-gauge-v1";
  bool operator==(const RulesetDescriptor &) const = default;
  static RulesetDescriptor Current();
  static RulesetDescriptor Legacy();
};
struct JudgeWindowProvenance { Judgement judgement=None; std::int64_t earlyMicros=0;
  std::int64_t lateMicros=0; bool operator==(const JudgeWindowProvenance &) const=default; };
struct PlayerOptionProvenance { std::string option="NORMAL"; std::optional<std::int64_t> seed;
  bool operator==(const PlayerOptionProvenance &) const=default; };
struct ScoreStageProvenance {
  std::string chartMd5; std::string chartSha256; int longNoteMode=0;
  std::optional<std::uint64_t> chartRandomSeed; std::optional<std::string> chartRandomPrng;
  std::vector<int> chartRandomValues; JudgeRankSource judgeRankSource=JudgeRankSource::Unknown;
  std::optional<int> sourceJudgeRank; std::vector<JudgeWindowProvenance> effectiveJudgeWindows;
  bool operator==(const ScoreStageProvenance &) const=default;
};
struct ScoreProvenance {
  static constexpr int kSchemaVersion = 1;
  int schemaVersion=kSchemaVersion; RulesetDescriptor ruleset;
  std::vector<ScoreStageProvenance> stages; GaugeType gaugeType=GaugeType::Normal;
  GaugeProfile gaugeProfile=GaugeProfile::Standard; bool gaugeAutoShift=false;
  PlayerOptionProvenance player1; PlayerOptionProvenance player2;
  std::string assistOption=assist_options::kOff;
  std::vector<InputDeviceCategory> inputDevices; bool autoPlay=false; bool practice=false;
  ScoreEligibility eligibility=ScoreEligibility::LegacyUnverified;
  bool operator==(const ScoreProvenance &) const=default;
  static ScoreProvenance Legacy();
};
```

Expose deterministic serialize/deserialize, `makeScoreProvenance(...)`, and `mergeCourseProvenance(std::span<const ScoreProvenance>)`. Sort and deduplicate device categories; preserve stage order and judge-window enum order.

- [ ] **Step 4: Verify and commit**

```bash
cmake --build cmake-build-debug --target score_provenance_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_provenance_contract$' --output-on-failure
git add src/AssistOptionUtils.h src/ReplayData.h src/ScoreProvenance.* tests/score_provenance_tests.cpp src/CMakeLists.txt CMakeLists.txt ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
git commit -m "feat(score): define immutable provenance"
```

---

### Task 2: Migrate score/replay schemas and persist provenance

**Files:**
- Modify: `src/ScoreDBHelper.h`
- Modify: `src/ScoreDBHelper.cpp`
- Modify: `src/ReplayDBHelper.h`
- Modify: `src/ReplayDBHelper.cpp`
- Modify: `src/ReplayData.h`
- Modify: `src/AppDatabaseInitializer.h`
- Modify: `tests/replay_db_helper_tests.cpp`
- Create: `tests/score_provenance_db_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 serialization and legacy value.
- Produces: path-bindable helpers, unified schema initialization, and persisted provenance for chart/course scores/replays.

- [ ] **Step 1: Write migration/database tests before DDL**

Create fixture databases in temporary directories by executing the exact v4 score and v2 replay schemas, inserting chart and course rows/events, and capturing outcome columns. Tests then call `EnsureSchema()` and assert:

```cpp
assert(databaseUserVersion(scoreDb) == 5);
assert(databaseUserVersion(replayDb) == 3);
assert(oldOutcomeSnapshot == readOutcomeSnapshot(scoreDb));
assert(readEligibility(scoreDb, "scores", oldId) == ScoreEligibility::LegacyUnverified);
assert(readRulesetVersion(scoreDb, "scores", oldId) == 0);
```

Add new chart/course round-trip equality, second-run idempotency, future-version no-mutation, and two helper instances bound to different paths.

- [ ] **Step 2: Make helpers path-bindable and fail closed**

Add constructors, `SetDatabasePath`, `GetDatabasePath`, and `EnsureSchema` to both helpers. Default empty path preserves current legacy locations. `SetDatabasePath()` increments score revision even without writes. Read `PRAGMA user_version` and reject a future value before any `CREATE`, `ALTER`, index, or trigger statement.

- [ ] **Step 3: Add migrations and columns**

For `scores`, `course_scores`, `replays`, and `course_replays`, add:

```sql
ruleset_version INTEGER NOT NULL DEFAULT 0,
eligibility INTEGER NOT NULL DEFAULT 2,
provenance_json TEXT NOT NULL DEFAULT '{"schemaVersion":1,"ruleset":{"version":0},"stages":[],"eligibility":"legacy-unverified"}'
```

Run each database migration in one transaction, add columns only when absent, preserve every existing outcome column, and update `user_version` last. Ensure both chart and course tables exist before the unified migration runs.

- [ ] **Step 4: Extend save/load APIs**

Add `const ScoreProvenance&` to chart/course `InsertScore` and `SaveScore` APIs. Add `ScoreProvenance provenance = ScoreProvenance::Legacy();` to `ReplayData` and `CourseReplayData`; add ruleset/eligibility to `ReplaySummary`. Serialize on save and deserialize on load, failing the row load with a diagnostic if a new record contains invalid provenance.

- [ ] **Step 5: Verify and commit**

```bash
cmake --build cmake-build-debug --target score_provenance_db_tests replay_db_helper_tests -j 6
ctest --test-dir cmake-build-debug -R '^(foundation_provenance_db|replay_db_helper_tests)$' --output-on-failure
git add src/ScoreDBHelper.* src/ReplayDBHelper.* src/ReplayData.h src/AppDatabaseInitializer.h tests CMakeLists.txt
git commit -m "feat(score): persist score and replay provenance"
```

---

### Task 3: Capture provenance once at play start

**Files:**
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/CoursePlaySession.h`
- Modify: `tests/score_provenance_tests.cpp`

**Interfaces:**
- Consumes: Task 2 persistence plus device categories from input resolver when available.
- Produces: identical immutable provenance saved with each score/replay and ordered aggregate course provenance.

- [ ] **Step 1: Extend tests with immutable attempt behavior**

Create a pure helper exercised by tests: capture from `StartOptions`, effective judge windows, and chart metadata; mutate the source options/settings afterwards; assert captured JSON is unchanged. Add a constrained-course test proving effective windows differ and eligibility is Modified, plus a test proving score and replay receive equal values.

- [ ] **Step 2: Add attempt inputs and course aggregation**

Extend `StartOptions` with:

```cpp
std::vector<InputDeviceCategory> inputDeviceCategories;
std::optional<RulesetDescriptor> rulesetDescriptor;
```

Extend `CoursePlaySession` with one `RulesetDescriptor`, ordered `stageProvenance`, `recordStageProvenance(index,value)`, and `aggregateProvenance()`.

- [ ] **Step 3: Capture after effective judge constraints**

`GamePlayScene` chooses the course descriptor or `RulesetDescriptor::Current()`. After constructing `Judge` and applying course constraints, build `attemptProvenance` from `judge.timingWindows`. `beginReplayRecording()` copies that existing value into the replay and never rebuilds from mutable settings. Until the input branch is integrated, desktop defaults to Keyboard and mobile defaults to Touch; integration later replaces this with resolver categories.

- [ ] **Step 4: Save the same value everywhere**

Add a required `ScoreProvenance attemptProvenance` constructor argument/member to `ResultScene`; gameplay passes its immutable member even when replay persistence is disabled. `ResultScene::saveScore()` passes that member, and `saveReplay()` asserts/copies the equal member into the replay. Course stage recording stores the value by index; course score and top-level course replay use `aggregateProvenance()`. Replay playback remains non-scoring. Practice/autoplay values may exist in memory but remain Modified.

- [ ] **Step 5: Verify and commit**

```bash
cmake --build cmake-build-debug --target score_provenance_tests score_provenance_db_tests main -j 6
ctest --test-dir cmake-build-debug -R '^foundation_provenance_' --output-on-failure
git add src/scene src/CoursePlaySession.h tests/score_provenance_tests.cpp
git commit -m "feat(score): snapshot provenance at play start"
```
