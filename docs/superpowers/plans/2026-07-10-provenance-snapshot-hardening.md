# Provenance Snapshot Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject future SQLite databases without touching their original recovery files, return replay summaries from one coherent snapshot, and omit every unloadable course aggregate.

**Architecture:** A shared raw/isolated-WAL-snapshot preflight approves schema ownership before either helper opens the original database read-write. Replay list methods hold a deferred read transaction across bounded candidate validation and detail hydration. Course-stage validation requires one or more coherent linked replays.

**Tech Stack:** C++23, repository-amalgamated SQLite, `std::filesystem`, POSIX fork/locking fixtures on the existing non-Windows test path, CMake/CTest.

## Global Constraints

- Work only in `/Users/xf/workspace/SNURhythm/AsoBMaShow/.worktrees/foundation-provenance` on `feature/foundation-provenance`; do not merge.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`.
- Preserve the score savepoint fix, positive summary budget of `limit + 512`, 64-row chunks, aggregate diagnostics, and deferred event/touch counts.
- Never open the original database read-write until preflight establishes a supported WAL-visible `user_version`.
- Fail closed on unreadable or ambiguous preflight.
- Use strict RED/GREEN cycles and raw snapshots that never normalize the original fixture before capture.
- Do not add a new `.cpp` source file; keep the shared preflight header-only so iOS membership exceptions remain unchanged.

---

### Task 1: Non-mutating schema preflight

**Files:**
- Modify: `src/SqliteRAII.h`
- Modify: `src/ScoreDBHelper.cpp`
- Modify: `src/ReplayDBHelper.cpp`
- Modify: `tests/score_provenance_db_tests.cpp`
- Modify: `tests/replay_db_helper_tests.cpp`

**Interfaces:**
- Produces: `preflightSqliteUserVersion(path, maximumVersion, error)` returning an approved version only when the original family can be inspected without mutation.
- Consumes: existing `openSqliteDatabase()` only after approval.

- [x] **Step 1: Add raw family fixtures**

Add a byte-only snapshot of `path`, `path-journal`, `path-wal`, and `path-shm`. Add clean DELETE, child-`_exit` WAL, and hot rollback-journal future fixtures. For every fixture, take the before snapshot without SQLite open, invoke score/replay `Connect()` or a high-level save, take the after snapshot without SQLite open, and assert exact equality and rejection.

```cpp
struct RawDatabaseFamilySnapshot {
  std::array<std::optional<std::string>, 4> files;
  bool operator==(const RawDatabaseFamilySnapshot &) const = default;
};

const auto before = rawDatabaseFamilySnapshot(path);
ScoreDBHelper helper(path);
assert(helper.Connect() == nullptr);
assert(rawDatabaseFamilySnapshot(path) == before);
```

- [x] **Step 2: Run RED**

```sh
cmake --build cmake-build-debug --target score_provenance_db_tests replay_db_helper_tests -j 6
ctest --test-dir cmake-build-debug -R '^(foundation_provenance_db|replay_db_helper_tests)$' --output-on-failure
```

Expected: child-exit WAL fixtures show the main file checkpointed and WAL/SHM removed by current `Connect()`.

- [x] **Step 3: Add shared preflight**

Implement a clean raw-header fast path and isolated WAL snapshot path in `SqliteRAII.h`. Reject rollback journals as ambiguous. For WAL state, write only page 1 into a sparse logical-size main snapshot, copy the WAL to a unique private temporary directory, recover/query the copy with bundled SQLite, clean it on every exit, and require unchanged original WAL bytes, page 1, and family presence/size/write-time state. Bound the Windows non-sparse fallback and reject an active writer after schema support is established. Return no approved version for malformed/negative headers, copy/query errors, changing family state, or a visible version above the caller's maximum.

Update both `Connect()` methods:

```cpp
std::string preflightError;
if (!preflightSqliteUserVersion(path, kSupportedVersion, preflightError)
         .has_value()) {
  SDL_Log("Refusing to open ...: %s", preflightError.c_str());
  return nullptr;
}
sqlite3 *db = openSqliteDatabase(path, openError);
```

- [x] **Step 4: Run GREEN**

Run the focused command from Step 2. Confirm every raw family is identical after rejection, then commit the preflight and tests.

---

### Task 2: Coherent summary transactions

**Files:**
- Modify: `src/ReplayDBHelper.cpp`
- Modify: `tests/replay_db_helper_tests.cpp`

**Interfaces:**
- Consumes: `SqliteTransactionHandle` and current bounded candidate scan.
- Produces: one SQLite read snapshot covering candidate validation, stage validation, and detail hydration.

- [x] **Step 1: Add deterministic WAL mutation regressions**

Create large chart/course candidate sets with padded valid provenance. A forked writer waits until the listing process holds a WAL read lock, commits `ruleset_version=99` for the newest already-visible candidate, and exits. Assert the returned summary is either the original coherent value or absent, never 99; assert a later call rejects the now-invalid row.

```cpp
const auto summaries = helper.ListReplays(meta, candidateCount);
assert(std::none_of(summaries.begin(), summaries.end(), [](const auto &row) {
  return row.rulesetVersion > RulesetDescriptor::kCurrentVersion;
}));
```

- [x] **Step 2: Run RED**

Build and run `replay_db_helper_tests`. Expected: the current two-phase implementation returns the writer's future indexed value during detail hydration.

- [x] **Step 3: Hold one read transaction**

Start a deferred transaction after `CreateReplayTables()` and before preparing/stepping candidate statements. Keep it alive through detail hydration. Commit before a successful return; all early returns rely on RAII rollback and log the failing phase.

- [x] **Step 4: Run GREEN and retain bounded behavior**

Run `replay_db_helper_tests`, including the existing 600-row corruption-budget fixture. Confirm one aggregate diagnostic, 513 inspected candidates for `limit=1`, and no per-row amplification.

---

### Task 3: Loadable course-stage contract

**Files:**
- Modify: `src/ReplayDBHelper.cpp`
- Modify: `tests/replay_db_helper_tests.cpp`

**Interfaces:**
- Consumes: reusable `courseReplayStagesHaveValidProvenance()` statement.
- Produces: true only when `1..256` linked stage rows exist and every linked replay provenance tuple is coherent.

- [x] **Step 1: Add invalid-structure regressions**

Insert aggregates representing zero stages, a stage link whose replay was deleted with foreign keys disabled, malformed JSON, a future ruleset JSON/index tuple, and an indexed mismatch. Add one valid aggregate older than them. Assert both `limit=1` and `limit=0` return only the valid aggregate and every invalid aggregate fails `LoadCourseReplay()`.

- [x] **Step 2: Run RED**

Run `replay_db_helper_tests`. Expected: the zero-stage aggregate appears in both list modes.

- [x] **Step 3: Require at least one stage**

Return false with a bounded aggregate rejection reason when the reusable stage query produces zero rows. Preserve missing replay, invalid provenance, SQL failure, and 256-stage ceiling handling.

- [x] **Step 4: Run GREEN**

Run `replay_db_helper_tests` and update the legacy list-limit fixture to create valid linked stages rather than relying on unloadable zero-stage rows.

---

### Task 4: Verification, review, and handoff

**Files:**
- Create outside branch: `.superpowers/sdd/provenance-task-2-snapshot-fix-report.md`

**Interfaces:**
- Produces: pinned commit and evidence for integration/re-review.

- [ ] **Step 1: Run focused and adversarial verification**

```sh
cmake --build cmake-build-debug --target \
  score_provenance_db_tests replay_db_helper_tests score_provenance_tests \
  app_database_initializer_tests gbattle_tests main -j 6
ctest --test-dir cmake-build-debug \
  -R '^(foundation_provenance_contract|foundation_provenance_db|replay_db_helper_tests|app_database_initializer_tests|gbattle_tests)$' \
  --output-on-failure
```

- [ ] **Step 2: Run full verification**

```sh
ctest --test-dir cmake-build-debug --output-on-failure -j 6
clang-format --dry-run --Werror \
  src/SqliteRAII.h src/ScoreDBHelper.cpp src/ReplayDBHelper.cpp \
  tests/score_provenance_db_tests.cpp tests/replay_db_helper_tests.cpp
git diff --check
plutil -lint ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
```

- [ ] **Step 3: Request independent review**

Review the final commit against all three Important findings. Fix every Critical/Important issue under another RED/GREEN cycle.

- [ ] **Step 4: Commit and report**

Commit the production/tests change with `fix(score): preserve SQLite provenance snapshots`. Write the requested integration report with RED/GREEN evidence, performance/log evidence, full-suite counts, commit SHA, and clean-worktree status. Do not merge.
