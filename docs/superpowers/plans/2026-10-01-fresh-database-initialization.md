# Fresh database initialization implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task by task.

**Goal:** Avoid historical migrations on fresh score/replay databases and repeated score repair scans on reopening.

**Architecture:** Detect an empty, unversioned schema inside the initialization transaction and create current tables and indexes directly. Retain all beta migrations. Persist completion of score data repairs separately from user_version because duration migration can remain deferred.

**Tech Stack:** C++, SQLite, CMake/CTest.

**Spec:** User-approved fixes from the migration review in this conversation.

## Global constraints

- Work on a new branch in the existing checkout; no worktree or deployment.
- Preserve beta data, schema numbers, future-version rejection, and transaction rollback.
- Preserve surrounding formatting; commit and push only task changes.

## Review focus

- Existing beta databases must retain their upgrade path and records.
- Deferred duration migrations must still retry after chart metadata becomes available.
- Fresh construction must produce the same usable schema as upgrades.
- Failed construction or repair must roll back its completion marker and writes.
- Nonempty or future databases must never take the fresh-install shortcut.

## Tasks

- [x] Add SQL trace regression tests in score_provenance_db_tests and replay_legacy_migration_tests; observe failures for legacy rewrites on fresh initialization and repeated score UPDATEs on reopen.
- [x] Add transactional fresh replay initialization using existing current SQL constants.
- [x] Add transactional fresh score initialization using shared table/index definitions; persist repair completion without bypassing deferred duration migrations.
- [x] Test repair rollback/retry and compare fresh and migrated schemas; retain existing future-version, malformed-schema, migration, and deferred-retry tests.
- [x] Build targeted tests and main; run CTest with -j 6 and review the diff.

Delivery: commit verified task files and push the new branch to origin.

## Execution notes

The user explicitly requested implementation and continuation, so execution proceeds in this session. No additional plan approval is needed. Initial checkout was clean on develop; created fix/fresh-database-initialization.

Regression evidence: fresh score initialization failed on historical ALTER TABLE, fresh replay initialization failed on creating raw replay tables, and current score reopening failed on the repeated BP UPDATE before implementation. Schema parity caught and corrected the missing course-key/LN-mode index. Independent review caught BP loss across the nullable-SHA rebuild; the added legacy fixture reproduced it, and BP repair now runs after that rebuild. Fresh creation and repair completion are covered by failed-commit rollback/retry tests. The reviewer verified the correction and found no remaining issues.

Final validation: `cmake --build cmake-build-debug -j 6` succeeded, including main and all test binaries. `ctest --test-dir cmake-build-debug --output-on-failure -j 6` passed all 400 tests (175.61 seconds). `git diff --check` passed. Independent read-only review found no remaining issues after the nullable-SHA correction.
