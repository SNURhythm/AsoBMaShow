# Replay File Actions and Profile Transfer Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:test-driven-development while executing this plan inline, one independently reviewable task at a time.

**Goal:** Activate safe share/delete actions, startup reconciliation, and atomic verified replay-file transfer for profile duplication and archives without coupling modern result history to file availability.

**Architecture:** Add a durable user-deleted state to the exact modern replay reference, project it through chart/course contexts and the capability matrix, and put all mutations behind one replay-file action/reconciliation boundary. Add one snapshot-driven profile transfer boundary used by duplication and archive import/export so only active metadata-verified BRDs cross profiles while all database result records remain intact.

**Tech Stack:** C++23, SQLite, `std::filesystem`, existing BRD codec/file store, existing profile staging and archive infrastructure, CMake/CTest.

---

### Task 1: Pin durable replay availability and mutation contracts

**Files:**
- Create: `tests/replay_file_action_service_tests.cpp`
- Modify: `tests/chart_replay_context_tests.cpp`
- Modify: `tests/course_replay_context_tests.cpp`
- Modify: `tests/replay_repository_modern_chart_tests.cpp`
- Modify: `tests/replay_repository_modern_course_tests.cpp`
- Modify: `CMakeLists.txt`

- [ ] Add failing tests proving a user-deleted reference is projected without
  file I/O, modern result and IR rows survive deletion, exact owner/reference
  mismatch fails closed, verified files may be shared, and corrupt/mismatched
  present files remain deletable.
- [ ] Build and run the focused targets to confirm failure is caused by the
  missing tombstone/action APIs.
- [ ] Commit the red contract tests independently.

### Task 2: Add the durable reference transition and shared action service

**Files:**
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryModernResults.cpp`
- Modify: `src/repositories/ReplayRepositorySchema.cpp`
- Modify: `src/replay/ChartReplayContext.{h,cpp}`
- Modify: `src/replay/CourseReplayContext.{h,cpp}`
- Create: `src/replay/ReplayFileActionService.{h,cpp}`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

- [ ] Migrate schema 12 to 13 transactionally by rebuilding
  `modern_replay_files` with strict `user_deleted IN (0,1)` and preserving all
  existing rows as active.
- [ ] Add `userDeleted` to the shared reference and exact chart/course mutation
  APIs that compare owner, attempt, path identity, and metadata before setting
  the tombstone.
- [ ] Add strict snapshot enumeration for active/tombstoned references.
- [ ] Project `UserDeleted` in both contexts before any store read.
- [ ] Implement share preparation and tombstone-first deletion through one
  chart/course action service; retry physical removal idempotently.
- [ ] Run repository, context, capability, and action tests; commit.

### Task 3: Add conservative startup reconciliation

**Files:**
- Create: `src/replay/ReplayFileReconciler.{h,cpp}`
- Modify: `src/ApplicationStartup.{h,cpp}`
- Modify: `src/main.cpp`
- Modify: `tests/application_startup_tests.cpp`
- Create: `tests/replay_file_reconciler_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

- [ ] First add failing tests for startup ordering, stale private temporary
  cleanup, tombstoned-file retry, missing-file tolerance, and preservation of
  active corrupt/unreferenced files.
- [ ] Implement a non-fatal reconciliation report and run it after database
  initialization but before the ready application callback.
- [ ] Run focused startup/reconciler tests; commit.

### Task 4: Activate Records share and delete actions

**Files:**
- Modify: `src/scene/MainMenuScene.{h,cpp}`
- Modify: `tests/result_record_list_view_tests.cpp`
- Modify: `tests/modern_chart_persistence_contract_tests.cpp`
- Modify: `tests/modern_course_persistence_contract_tests.cpp`

- [ ] Add failing source/behavior contracts requiring button visibility to use
  only `ReplayCapabilities`, sharing to use a verified action-service snapshot,
  and deletion to reload records without deleting the result.
- [ ] Wire platform document handoff lifetime/polling and a confirmation-backed
  delete action for modern chart/course selections only.
- [ ] Run focused UI contract tests and commit.

### Task 5: Transfer verified owned BRDs during profile duplication

**Files:**
- Modify: `src/PlayerProfile.h`
- Modify: `src/PlayerProfileManager.{h,cpp}`
- Create: `src/replay/ReplayProfileTransfer.{h,cpp}`
- Modify: `tests/player_profile_manager_tests.cpp`
- Create: `tests/replay_profile_transfer_tests.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`

- [ ] Add failing tests for verified, missing, user-deleted, corrupt,
  mismatched, and unreferenced files, including injected copy/verify/sync/final
  failures and complete staging rollback.
- [ ] Add `replayDirectory` to profile paths and create it for every profile.
- [ ] Implement strict snapshot inventory and verified active-file copy into
  staging. Omit missing/user-deleted bytes, fail on invalid referenced bytes,
  and ignore source unreferenced bytes.
- [ ] Integrate the boundary after the replay database snapshot and before
  final staging sync; run focused tests and commit.

### Task 6: Add profile archive format 3 replay members

**Files:**
- Modify: `src/ProfileArchive.{h,cpp}`
- Modify: `tests/profile_archive_tests.cpp`

- [ ] Add failing export/import and fault tests for the same six file states,
  archive versions 1/2 compatibility, extra/unsafe member rejection, exact
  reference agreement, and overwrite rollback.
- [ ] Export format 3 with only verified active referenced BRDs from the
  sanitized replay snapshot.
- [ ] Import replay members into staging and validate them against the staged
  replay database before the existing atomic install boundary.
- [ ] Run all profile archive/manager/transfer tests and commit.

### Task 7: Slice gate and shared-boundary review

- [ ] Run all replay capability, store, repository, context, action,
  reconciliation, profile manager, and archive tests plus desktop `main`.
- [ ] Review `git diff origin/develop...HEAD` for path validation, reference
  agreement, or availability logic duplicated across chart/course/profile/UI.
- [ ] Move any duplicate authority to the shared boundary and add a regression
  test before the fix.
- [ ] Update the contract matrix status and commit the Slice 6 gate.

Execution is inline in the current branch, as explicitly requested. No pause or
handoff is required before Slice 7.
