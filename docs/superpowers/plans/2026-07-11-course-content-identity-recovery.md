# Course Content Identity and Record Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make course lamps, scores, and replays content-addressed and recover records orphaned by difficulty-table course-ID churn.

**Architecture:** A shared `CourseIdentity` module computes `course:v1:<sha256>` from ordered chart hashes and semantic static constraints. Chart courses and play sessions carry that key; score and replay databases migrate/backfill it and use it before numeric navigation IDs. A conservative recovery pass matches historical score/replay evidence against current chart-course definitions under each database's existing singleton transaction.

**Tech Stack:** C++23, SQLite, nlohmann JSON, CMake/CTest, existing `FileChecksum` SHA-256 and profile database activity guards.

## Global Constraints

- Course name, table/group/level IDs, display metadata, trophy conditions, and grade option restrictions are not identity.
- Ordered charts plus no-speed/judgement/gauge/forced-LN constraints are identity.
- SHA-256 is preferred; MD5 is the compatibility fallback; paths are never new durable identity.
- New score rows store exact LN mode; legacy unknown LN mode uses `-1` and matches every selected mode.
- Numeric fallback applies only to rows with an empty content key.
- Recovery never guesses ambiguous or corrupt rows and never deletes historical rows.
- Score migration preserves raw legacy keys permanently, and recovery never rewrites the historical ID/name/group/constraint/chart-count tuple.
- Replay recovery updates only the content key; partial replay stages must be a contiguous prefix and historical replay tuple fields remain unchanged.
- Preserve the current numeric-ID retention change as compatibility protection.
- Keep Yoga tests excluded from CTest.

---

### Task 1: Shared canonical course identity

**Files:**
- Create: `src/CourseIdentity.h`
- Create: `src/CourseIdentity.cpp`
- Create: `tests/course_identity_tests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj`

**Interfaces:**
- Produces:
  - `course_identity::ChartIdentity { sha256, md5 }`
  - `course_identity::Definition { courseId, courseKey, name, groupName, constraintJson, charts }`
  - `course_identity::canonicalConstraintPayload(std::string_view)`
  - `course_identity::makeCourseKey(std::span<const ChartIdentity>, std::string_view)`
  - `course_identity::makeCourseKey(const CoursePlaySession &)`
  - `course_identity::parseLegacyScoreKey(std::string_view)`
  - `course_identity::sameChart/sameDefinition/prefixMatches`

- [ ] **Step 1: Write identity RED tests**

```cpp
expect(makeCourseKey(charts, R"(["no_speed","gauge_7k"])") ==
       makeCourseKey(charts, R"(["gauge-7k","NO SPEED"])") ,
       "constraint formatting and order are canonical");
expect(makeCourseKey(charts, "[]") == makeCourseKey(charts, R"(["grade"])") ,
       "grade restriction is not content identity");
expect(makeCourseKey(charts, "[]") != makeCourseKey(reversed, "[]"),
       "chart order is content identity");
expect(parseLegacyScoreKey(legacyNamedKey)->courseKey ==
       makeCourseKey(charts, constraints),
       "legacy score key drops course name");
```

- [ ] **Step 2: Register and run the test to verify RED**

Run: `cmake --build cmake-build-debug --target course_identity_tests -j 6 && ctest --test-dir cmake-build-debug -R '^course_identity_tests$' --output-on-failure`

Expected: build/link failure because `CourseIdentity` APIs do not exist.

- [ ] **Step 3: Implement canonical payload and digest**

```cpp
namespace course_identity {
struct ChartIdentity { std::string sha256; std::string md5; };
struct ParsedLegacyKey {
  std::string courseName;
  std::string constraintJson;
  std::vector<ChartIdentity> charts;
  std::string courseKey;
};

std::string makeCourseKey(std::span<const ChartIdentity> charts,
                          std::string_view constraintJson) {
  const std::string payload = serializeVersionedPayload(charts,
      canonicalConstraintPayload(constraintJson));
  return payload.empty() ? std::string() :
      "course:v1:" + file_checksum::sha256(payload);
}
} // namespace course_identity
```

Canonicalize recognized constraints by type and fixed order; exclude grade and unknown constraints. Reject empty/malformed chart identities instead of producing a key.

- [ ] **Step 4: Link the implementation everywhere DB helpers compile**

Add `src/CourseIdentity.cpp` and `src/FileChecksum.cpp` to the focused score/replay/profile targets that compile helpers directly. Add `CourseIdentity.cpp` and `CourseIdentity.h` to iOS membership exceptions.

- [ ] **Step 5: Run GREEN and formatting checks**

Run: `cmake --build cmake-build-debug --target course_identity_tests -j 6 && ctest --test-dir cmake-build-debug -R '^course_identity_tests$' --output-on-failure && git diff --check`

Expected: one test target passes; no whitespace errors.

### Task 2: Store canonical keys on chart courses and sessions

**Files:**
- Modify: `src/ChartDBHelper.h`
- Modify: `src/ChartDBHelper.cpp`
- Modify: `src/CoursePlaySession.h`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Test: `tests/profile_switch_tests.cpp`

**Interfaces:**
- Produces: `ChartDBHelper::SelectDifficultyCourseDefinitions(sqlite3 *)`
- Consumes: Task 1 identity helpers.

- [ ] **Step 1: Replace the temporary ID-only regression with RED content-key cases**

```cpp
// Import course A, capture {id,key}; refresh with renamed course/metadata.
expect(refreshed.key == initial.key, "renamed course keeps content key");
// Change chart order or gauge constraint.
expect(changed.key != refreshed.key, "changed definition gets a new key");
```

Also assert a score/replay-facing `CoursePlaySession` can carry the selected course key independently of `courseId`.

- [ ] **Step 2: Run RED**

Run: `cmake --build cmake-build-debug --target profile_switch_tests -j 6 && ctest --test-dir cmake-build-debug -R '^foundation_profile_switch$' --output-on-failure`

Expected: failure because `difficulty_courses.course_key` and public key fields do not exist.

- [ ] **Step 3: Migrate and populate chart course keys**

```sql
ALTER TABLE difficulty_courses
ADD COLUMN course_key TEXT NOT NULL DEFAULT '';
CREATE INDEX IF NOT EXISTS idx_difficulty_courses_key
ON difficulty_courses(course_key);
```

Backfill blank keys from ordered course entries. Insert `course_key` for new courses. When strongest-common matching retains an old definition, retain its nonempty key and ID. Remove course name from the retention comparison.

- [ ] **Step 4: Expose keys and definitions**

Add `courseKey` to `DifficultyCourseInfo`, singleton group metadata, `LibraryFolderItem`, and `CoursePlaySession`. Set the session key from the selected chart course. Return current definitions with both hashes for conservative recovery.

- [ ] **Step 5: Run GREEN**

Run the focused profile-switch test and `git diff --check`.

### Task 3: Score schema v6, recovery, and key-authoritative reads

**Files:**
- Modify: `src/ScoreDBHelper.h`
- Modify: `src/ScoreDBHelper.cpp`
- Modify: `src/scene/MainMenuLibrary.cpp`
- Modify: `src/scene/MainMenuLibrary.h` if present
- Test: `tests/score_provenance_db_tests.cpp`
- Test: `tests/main_menu_library_tests.cpp`

**Interfaces:**
- Produces: `CourseScoreRecoveryResult ScoreDBHelper::RecoverCourseRecords(std::span<const course_identity::Definition>)`
- `CourseScoreRecoveryResult` includes canonical score evidence required by replay recovery.

- [ ] **Step 1: Write score migration/read RED tests**

Cover:

```cpp
expect(legacyV5MigratesToVersion6());
expect(storedCourseKey() == canonicalKey);
expect(storedLegacyCourseKey() == rawLegacyKey);
expect(storedCourseLnMode() == -1); // unknowable legacy selection
expect(loadBestCourseScore(newId, canonicalKey).has_value());
expect(!loadBestCourseScore(sameId, differentNonemptyKey).has_value());
expect(bestCourseLamp(canonicalKey, selectedLnMode) == recoveredRank);
```

Add a main-menu fixture where the course ID and name change while content key remains constant; the lamp must remain.

- [ ] **Step 2: Run RED**

Run: `cmake --build cmake-build-debug --target score_provenance_db_tests main_menu_library_tests -j 6 && ctest --test-dir cmake-build-debug -R 'foundation_provenance_db|main_menu_library_tests' --output-on-failure`

Expected: schema/key/lamp assertions fail.

- [ ] **Step 3: Implement atomic v5-to-v6 migration**

Add `legacy_course_key TEXT NOT NULL DEFAULT ''` and `ln_mode INTEGER NOT NULL DEFAULT -1`. Copy every noncanonical raw key to `legacy_course_key` before converting parseable keys, retain that evidence permanently, create `(course_key, ln_mode, clear_type)` index, and set `user_version=6` only on commit. Leave malformed keys unchanged and all rows intact.

- [ ] **Step 4: Write exact new score identity**

Use `session.courseKey` or Task 1 fallback, store exact normalized `session.longNoteMode`, and refuse an empty key for a new course score.

- [ ] **Step 5: Make reads and lamps key-authoritative**

```sql
WHERE course_key = :key
  AND (ln_mode = :ln_mode OR ln_mode = -1)
```

Group lamps by key and LN mode; maximize across actual play options. Permit `course_id` only for rows whose key is empty. Main-menu folders map `difficulty_courses.course_key` back to their numeric UI folder keys.

- [ ] **Step 6: Implement conservative score association recovery**

Parse the persistent raw legacy evidence, strongest-common match it against current definitions, and rewrite only `course_key` when all matches collapse to one content key. Preserve the historical ID/name/group/constraint/chart-count tuple and return owned, deduplicated tuple-to-key evidence for replay repair, including multiple keys when the tuple itself is ambiguous.

- [ ] **Step 7: Run GREEN**

Run both focused targets and their CTest entries.

### Task 4: Replay schema v4, complete backfill, and key lookup

**Files:**
- Modify: `src/ReplayData.h`
- Modify: `src/ReplayDBHelper.h`
- Modify: `src/ReplayDBHelper.cpp`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/scene/MainMenuScene.cpp`
- Test: `tests/replay_db_helper_tests.cpp`

**Interfaces:**
- Produces: `ReplayDBHelper::RecoverCourseRecords(definitions, scoreEvidence)`
- Changes list lookup to accept `{courseKey, legacyCourseId}`.

- [ ] **Step 1: Write replay RED tests**

Cover new-save full key with a valid partial stage prefix, rejection of missing-middle-stage compaction, v3-to-v4 strict complete backfill, key lookup after ID change, mismatching nonempty key refusing numeric fallback, blank legacy fallback, and incomplete/corrupt rows remaining intact.

- [ ] **Step 2: Run RED**

Run: `cmake --build cmake-build-debug --target replay_db_helper_tests -j 6 && ctest --test-dir cmake-build-debug -R '^replay_db_helper_tests$' --output-on-failure`

Expected: missing schema/API assertions fail.

- [ ] **Step 3: Implement replay v4 migration**

Add `course_key TEXT NOT NULL DEFAULT ''` before creating its index. Backfill only rows where `completed_charts == total_charts == contiguous stage count` and every stage has valid hash identity in the migration transaction. Bump version after successful commit.

- [ ] **Step 4: Persist and query the full session key**

Set `CourseReplayData.courseKey` from the full `CoursePlaySession.courseKey` in `ResultScene` before reducing stages. Store exactly stages `[0, completed_charts)` and reject a gap rather than compacting later stages. Save/load the key and list with:

```sql
WHERE course_key = :key
   OR (course_key = '' AND course_id = :legacy_id)
```

- [ ] **Step 5: Recover partial replays conservatively**

Match canonical constraints, `total_charts`, and ordered recorded prefix against current definitions. Only after that candidate set exists, use the exact immutable legacy score tuple evidence to disambiguate. Update only `course_key`, assign only one distinct content key, and leave ambiguous rows blank.

- [ ] **Step 6: Run GREEN**

Run the complete replay helper suite.

### Task 5: Profile migration lifecycle and recovery orchestration

**Files:**
- Modify: `src/PlayerProfileManager.cpp`
- Modify: `src/scene/MainMenuScene.cpp`
- Test: `tests/player_profile_manager_tests.cpp`
- Test: `tests/profile_archive_tests.cpp`
- Test: `tests/profile_switch_tests.cpp`

**Interfaces:**
- Consumes current definitions and both recovery helper APIs.

- [ ] **Step 1: Write RED lifecycle tests**

Assert supported v5/v3 profile databases are accepted for migration before strict validation, become v6/v4, remain isolated across profile switches, and future v7/v5 databases are rejected without mutation.

- [ ] **Step 2: Run RED**

Run the profile manager/archive/switch targets and matching CTest entries.

- [ ] **Step 3: Allow supported older versions through preflight**

Use a context-specific preflight policy: activation/import may admit supported v5/v3 databases only when binding immediately performs the atomic migration; inactive archive export remains strict-current. Keep future-version rejection and integrity checks.

- [ ] **Step 4: Orchestrate recovery before cache load**

In `MainMenuScene::reloadScoreClearRanks`, hold one outer profile-database activity guard, detach any prior score attachment, select current definitions, run score recovery, pass returned evidence to replay recovery, then prepare/attach and load score caches. This order avoids a score-mutex self-deadlock and prevents a profile switch between the two repairs. Log recovery failures, keep original rows, do not recover from the pre-commit profile-switch cache callback, and do not open a second connection to either profile database.

- [ ] **Step 5: Run GREEN**

Run all three focused profile suites.

### Task 6: Full verification and review

**Files:**
- Review all modified files.

- [ ] **Step 1: Format and static diff checks**

Run: `git clang-format --diff HEAD -- <changed C++ files>` and `git diff --check`.

- [ ] **Step 2: Build and run application-only CTest**

Run: `cmake --build cmake-build-debug -j 6 && ctest --test-dir cmake-build-debug --output-on-failure`

Expected: all registered application tests pass; no Yoga test appears in `ctest -N`.

- [ ] **Step 3: Cross-platform build check**

Run: `scripts/ios_firebase_deploy.sh --build-only --skip-init`

Expected: unsigned iOS build succeeds; no upload occurs.

- [ ] **Step 4: Independent code review**

Review identity semantics, migration rollback/future-version safety, profile singleton connection rules, recovery ambiguity, and main-menu/replay regressions. Resolve every actionable finding and repeat affected tests.
