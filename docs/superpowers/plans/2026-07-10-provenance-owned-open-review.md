# Provenance Owned-Open Review Remediation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make schema validation/open one guarded ownership operation, propagate every version/query error, bound isolated snapshot work, and make course summaries/full loads share one exact loadability contract.

**Architecture:** `SqliteRAII.h` will provide bounded isolated validation plus an exclusive guard that revalidates and configures the database before opening the returned lazy handle. Replay code will use one reusable ordered-stage reader and a connection-local replay loader for summaries and full course hydration.

**Tech Stack:** C++23, bundled SQLite 3.43.1, `std::filesystem`, POSIX fork fixtures, CMake/CTest.

## Global Constraints

- Work only in the existing `feature/foundation-provenance` worktree; do not merge.
- Preserve exact main/`-journal`/`-wal`/`-shm` bytes and presence for future/unsupported rejection.
- Do not edit amalgamated BMS parser files or add a new `.cpp` under `src`.
- Use RED before production changes for every finding.
- Preserve 64-row summary chunks, `limit + 512`, aggregate logging, and coherent WAL summary transactions.
- Keep temporary snapshot logical bytes and copy/compare I/O at or below 512 MiB.

---

### Task 1: Bounded usable snapshot and guarded owned open

**Files:**
- Modify: `src/SqliteRAII.h`
- Modify: `src/ScoreDBHelper.cpp`
- Modify: `src/ReplayDBHelper.cpp`
- Test: `tests/score_provenance_db_tests.cpp`
- Test: `tests/replay_db_helper_tests.cpp`

**Interfaces:**
- Produces: `readSqliteUserVersion(sqlite3 *, std::string &) -> std::optional<int>`.
- Produces: `openValidatedSqliteDatabase(path, maximumVersion, enableForeignKeys, error, hooks) -> sqlite3 *`.
- Consumes: raw family state and isolated-copy helpers already in `SqliteRAII.h`.

- [ ] **Step 1: Add WAL-without-SHM, clean-corrupt, budget, and pragma-error tests**

Create supported/future child-exit WAL fixtures, remove SHM without opening SQLite, and assert exact raw families for successful guarded open/close, future rejection, active writer, and injected no-checkpoint failure. Add a 4096-byte header-shaped corrupt clean file and assert both Connect methods return null without mutation. Add pure and sparse-file boundary tests for main+WAL+reserve at 512 MiB. Install a SQLite auto-extension authorizer that denies `journal_mode` and assert Connect returns null.

- [ ] **Step 2: Run RED**

```sh
cmake --build cmake-build-debug --target score_provenance_db_tests replay_db_helper_tests -j 6
ctest --test-dir cmake-build-debug \
  -R '^(foundation_provenance_db|replay_db_helper_tests)$' \
  --output-on-failure
```

Expected: supported WAL-without-SHM creates SHM/current preflight fails; corrupt clean header passes preflight; denied pragma still returns a connection; unbounded budget helpers do not exist.

- [ ] **Step 3: Add bounded exact-copy and usability validation**

Replace unbounded `copy_file`/EOF comparison with helpers that copy/compare exactly the measured size and reject an extra byte. Enforce overflow-safe `main + wal + 64 KiB <= 512 MiB`. For clean existing databases copy the complete main file; for WAL keep first-page sparse main. Query both `PRAGMA user_version` and `SELECT count(*) FROM sqlite_schema` on the isolated family.

- [ ] **Step 4: Add guarded open pair**

After isolated approval and the test hook, open a no-checkpoint guard, run `PRAGMA locking_mode=EXCLUSIVE; BEGIN EXCLUSIVE`, reread/range-check the visible version, roll back while retaining exclusive ownership, and set `journal_mode=WAL`. Open a second lazy production handle while the guard remains open; configure no-checkpoint, busy timeout, and optional foreign keys through C APIs. Close the guard only after the production handle is ready. On any failure, close all handles and return null.

- [ ] **Step 5: Route both Connect methods and run GREEN**

Replace separate preflight/open/pragma sequences with `openValidatedSqliteDatabase`. Run the focused command until both targets pass, then commit Task 1.

---

### Task 2: Error-aware transactional version guards

**Files:**
- Modify: `src/SqliteRAII.h`
- Modify: `src/ScoreDBHelper.cpp`
- Modify: `src/ReplayDBHelper.cpp`
- Test: `tests/score_provenance_db_tests.cpp`
- Test: `tests/replay_db_helper_tests.cpp`

**Interfaces:**
- Consumes: `readSqliteUserVersion` and isolated validation from Task 1.
- Produces: every schema/migration guard accepts only `0 <= version <= maximum`.

- [ ] **Step 1: Add negative and denied-PRAGMA transaction tests**

For score and replay, begin caller-owned transactions on negative-version fixtures and assert direct schema entry points reject with identical schema/version/row state. Install a connection authorizer that denies `PRAGMA user_version`, repeat inside a transaction, and assert no schema delta.

- [ ] **Step 2: Run RED**

Expected: current integer fallback accepts negative versions and may migrate them; denied reads are treated as version zero.

- [ ] **Step 3: Propagate optional versions**

Replace both local integer helpers with the shared optional reader. Update reject guards, migration entry points, and migration-pass loops to reject null/negative values explicitly. Preserve isolated validation for autocommit caller-owned handles and use the in-connection snapshot only when a caller transaction is active.

- [ ] **Step 4: Run GREEN and commit Task 2**

Build/run both focused targets and confirm every negative/error fixture remains unchanged.

---

### Task 3: Shared course stage structure and same-snapshot hydration

**Files:**
- Modify: `src/ReplayDBHelper.h`
- Modify: `src/ReplayDBHelper.cpp`
- Test: `tests/replay_db_helper_tests.cpp`

**Interfaces:**
- Produces: ordered stage descriptor reader used by list and load paths.
- Produces: connection-local replay loader used by `LoadReplay` and `LoadCourseReplay`.

- [ ] **Step 1: Add malformed-structure and step-error public regressions**

Create one older valid course behind empty-identity, partial-missing, indexes `0,2`, duplicate, negative-plus-valid, and 257-stage candidates. Assert limit-one and unlimited lists return only the valid course and full load returns null for every invalid id. Use an SQLite auto-extension trace callback to interrupt the stage SELECT and assert limited/unlimited list and full load fail closed on terminal step error.

- [ ] **Step 2: Run RED**

Expected: empty identity is advertised, while partial/gapped/negative-plus-valid/257-stage full loads return partial or default-filled data.

- [ ] **Step 3: Implement the shared descriptor reader**

Use one `LEFT JOIN` query selecting index, linked id, rest time, identity, and provenance. Enforce 1..256, exact contiguous indexes, present replay, nonempty trimmed identity, optional provenance coherence, and terminal `SQLITE_DONE`.

- [ ] **Step 4: Hydrate courses on one connection/snapshot**

Extract the body of `LoadReplay` into a connection-local helper. Make public `LoadReplay` delegate after Connect/schema setup. Make `LoadCourseReplay` hold one deferred read transaction across aggregate read, shared stage validation, and every stage replay/event/touch/lane-cover load; commit only after complete success.

- [ ] **Step 5: Run GREEN, retain bounded scans, and commit Task 3**

Run focused tests plus the existing 600-corrupt-row budget fixture. Confirm one aggregate log and the prior 513-candidate bound.

---

### Task 4: Documentation and final verification

**Files:**
- Modify: `docs/superpowers/specs/2026-07-10-provenance-snapshot-hardening-design.md`
- Modify outside branch: `.superpowers/sdd/provenance-task-2-snapshot-fix-report.md`

- [ ] **Step 1: Remove trailing whitespace and update architecture text**

Remove Markdown hard-break spaces and document the guarded open pair, clean isolated usability validation, 512 MiB aggregate budget, and shared course loader.

- [ ] **Step 2: Run focused stress and requested build**

```sh
cmake --build cmake-build-debug --target \
  score_provenance_db_tests replay_db_helper_tests score_provenance_tests \
  app_database_initializer_tests gbattle_tests main -j 6
ctest --test-dir cmake-build-debug \
  -R '^(foundation_provenance_contract|foundation_provenance_db|replay_db_helper_tests|app_database_initializer_tests|gbattle_tests)$' \
  --repeat until-fail:3 --output-on-failure
```

- [ ] **Step 3: Run full verification**

```sh
ctest --test-dir cmake-build-debug --output-on-failure -j 6
clang-format --dry-run --Werror \
  src/SqliteRAII.h src/ScoreDBHelper.cpp src/ReplayDBHelper.h \
  src/ReplayDBHelper.cpp tests/score_provenance_db_tests.cpp \
  tests/replay_db_helper_tests.cpp
git diff --check 4fa3794..HEAD
plutil -lint ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj
```

- [ ] **Step 4: Commit, update report, and request re-review**

Record RED/GREEN output, test counts/timings, final SHA/range, and clean status. Do not merge.
