# PR 82 Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve all six actionable PR #82 review threads without weakening replay durability, validation, or Beatoraja compatibility.

**Architecture:** Keep each fix at its existing ownership boundary: course session state reads decoded timing, the replay store owns durable slot replacement, the replay repository validates and returns references from the snapshotted database for profile export, the codec represents empty stock input, path construction enforces component limits, and repository profile binding triggers stale-temp cleanup. Each behavior gets an independent red-green regression test before production code changes.

**Tech Stack:** C++23 desktop tests, C++20 iOS application build, SQLite, filesystem-safe atomic replay operations, gzip/Base64URL Beatoraja BRD codec.

## Global Constraints

- Preserve Beatoraja's `replay/<stem>[_<history>].brd` layout.
- Never overwrite linked or non-regular replay paths.
- Replay slot replacement must write and sync a private temporary file before an atomic replacement.
- Profile export must use the snapshotted `replays.db` references and must not mutate the source profile.
- Empty input means a valid all-miss play; malformed partial key records remain invalid.
- Replay filename components must not exceed 255 bytes.
- Stale cleanup may remove only private replay temporary files older than one hour.

---

### Task 1: Raw Course Replay Rest Timing

**Files:**
- Modify: `tests/gameplay_ruleset_policy_tests.cpp`
- Modify: `src/CoursePlaySession.h`

**Interfaces:**
- Consumes: `replay::CourseReplayPlaybackData::restMicrosAfterStage`
- Produces: `CoursePlaySession::restMicrosAfterCurrentStage()` returning the decoded raw-course delay before legacy fallbacks

- [ ] **Step 1: Write the failing test**

Add a test that installs a `courseReplayPlaybackData` with `restMicrosAfterStage = {1'500'000, -1}`, selects each index, and asserts the first delay is returned while the negative delay is clamped to zero. Also retain a legacy fallback assertion.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build cmake-build-debug --target gameplay_ruleset_policy_tests -j 6 && cmake-build-debug/gameplay_ruleset_policy_tests`

Expected: FAIL because a session populated only through `courseReplayPlaybackData` returns zero.

- [ ] **Step 3: Write minimal implementation**

Check `courseReplayPlaybackData != nullptr` and `currentIndex < restMicrosAfterStage.size()` first, returning `std::max(0LL, restMicrosAfterStage[currentIndex])`; retain the judged legacy and freshly recorded fallbacks.

- [ ] **Step 4: Run test to verify it passes**

Run the Task 1 command and expect PASS.

### Task 2: Durable Occupied Slot Replacement

**Files:**
- Modify: `tests/replay_file_store_tests.cpp`
- Modify: `src/replay/ReplayFileStore.cpp`

**Interfaces:**
- Consumes: `ReplayFileStore::copyToBeatorajaSlot(source, stem, slot, diagnostic)`
- Produces: atomic replacement of a safe occupied visible slot with the validated source bytes

- [ ] **Step 1: Write the failing test**

Finalize two different valid BRDs, pre-populate visible slot `_1.brd` with the first bytes, then copy the second source into slot 1 and assert success plus exact second-source bytes at the destination.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build cmake-build-debug --target replay_file_store_tests -j 6 && cmake-build-debug/replay_file_store_tests`

Expected: FAIL with the existing `already contains different bytes` behavior.

- [ ] **Step 3: Write minimal implementation**

Validate an occupied destination with `privateRegularPath`; return early for identical bytes; write and fsync the source bytes to the existing private temporary path; atomically replace the destination with `atomic_file::renameDurably`; remove the temporary on failure; sync the replay directory after replacement.

- [ ] **Step 4: Run test to verify it passes**

Run the Task 2 command and expect PASS, including existing unsafe-link tests.

### Task 3: Archive Replay Reference Integrity

**Files:**
- Modify: `tests/profile_archive_tests.cpp`
- Modify: `src/ProfileArchive.cpp`
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryResultRecords.cpp`

**Interfaces:**
- Consumes: snapshotted `replays.db` columns `relative_path`, `content_sha256`, and `compressed_size`
- Produces: a repository-owned snapshot reference read and export failure with `ProfileError::IntegrityFailure` when any referenced staged replay is missing or differs from the database reference

- [ ] **Step 1: Write the failing test**

Create the normal compact replay fixture, replace its referenced `.brd` bytes without updating `replay_files`, export, and assert integrity failure and no committed archive.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build cmake-build-debug --target profile_archive_tests -j 6 && cmake-build-debug/profile_archive_tests`

Expected: FAIL because export currently succeeds and creates fresh archive checksums for corrupt bytes.

- [ ] **Step 3: Write minimal implementation**

Read and validate replay references from the already-created workspace snapshot through `ReplayRepository`, keeping raw SQLite access behind the repository boundary. During replay staging, validate each referenced staged file's exact size and SHA-256 and remove it from a remaining-reference set. Reject duplicate/malformed reference rows and reject export if any reference remains missing.

- [ ] **Step 4: Run test to verify it passes**

Run the Task 3 command and expect PASS, including deterministic export/import coverage.

### Task 4: Empty All-Miss Replay Input

**Files:**
- Modify: `tests/beatoraja_replay_codec_tests.cpp`
- Modify: `src/replay/BeatorajaReplayCodec.cpp`

**Interfaces:**
- Consumes: empty `ReplayPlaybackData::input`
- Produces: a valid gzip/Base64URL empty `keyinput` that decodes back to an empty transition vector for charts and courses

- [ ] **Step 1: Write the failing test**

Clear chart input, encode/decode it and a one-stage course, and assert both round-trip with an empty input vector.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build cmake-build-debug --target beatoraja_replay_codec_tests -j 6 && cmake-build-debug/beatoraja_replay_codec_tests`

Expected: FAIL because `encodeStockInput` rejects empty records.

- [ ] **Step 3: Write minimal implementation**

Allow `encodeStockInput` to return an empty record vector and allow `decodeStockInput` when decompression yields zero bytes; continue rejecting any nonzero byte count not divisible by the nine-byte record size.

- [ ] **Step 4: Run test to verify it passes**

Run the Task 4 command and expect PASS.

### Task 5: Replay Filename Component Bound

**Files:**
- Modify: `tests/beatoraja_replay_path_tests.cpp`
- Modify: `src/replay/BeatorajaReplayPath.h`
- Modify: `src/replay/BeatorajaReplayPath.cpp`

**Interfaces:**
- Produces: `pathForStem` rejecting any final filename over 255 bytes and `courseStem` reserving enough room for `_` plus the maximum signed history index and `.brd`

- [ ] **Step 1: Write the failing test**

Assert that a 23-stage no-prefix course remains representable at `INT64_MAX`, a 24-stage course is rejected, and a shorter stage list whose prefix/constraints exceed the remaining budget is rejected.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build cmake-build-debug --target beatoraja_replay_path_tests -j 6 && cmake-build-debug/beatoraja_replay_path_tests`

Expected: FAIL because 24- and 26-stage stems are currently accepted.

- [ ] **Step 3: Write minimal implementation**

Define a 255-byte component limit. After building a course stem, reject it if appending the maximum history suffix and `.brd` would exceed that limit. Independently enforce the actual formatted filename length in `pathForStem`.

- [ ] **Step 4: Run test to verify it passes**

Run the Task 5 command and expect PASS.

### Task 6: Stale Replay Temp Cleanup on Profile Binding

**Files:**
- Modify: `tests/replay_repository_v11_tests.cpp`
- Modify: `src/repositories/ReplayRepository.cpp`

**Interfaces:**
- Consumes: resolved replay database parent as the profile root
- Produces: stale replay-temp cleanup from both startup `SetDatabasePath` and profile activation `BindDatabasePath`

- [ ] **Step 1: Write the failing test**

Create stale `.<stem>.brd.<attempt>.tmp` files under two profile roots, call `SetDatabasePath` for the first and `BindDatabasePath` for the second, and assert stale files are removed while a recent temporary and unrelated file remain.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build cmake-build-debug --target replay_repository_tests -j 6 && cmake-build-debug/replay_repository_tests`

Expected: FAIL because repository path binding never calls cleanup.

- [ ] **Step 3: Write minimal implementation**

Add a private translation-unit helper that constructs `ReplayFileStore(resolvedDatabase.parent_path())` and calls `removeStaleTemporaryFiles(now - 1h)`. Invoke it after `SetDatabasePath` and after successful same-path or replacement `BindDatabasePath` validation.

- [ ] **Step 4: Run test to verify it passes**

Run the Task 6 command and expect PASS.

### Task 7: Integrated Verification and PR Update

**Files:**
- Verify all modified production and test files
- Update PR #82 with one review-fix commit

**Interfaces:**
- Produces: a clean branch whose local desktop and iOS builds cover all fixes

- [ ] **Step 1: Format and inspect**

Run `clang-format -i` on the modified C++ files, then `git diff --check` and inspect `git diff`.

- [ ] **Step 2: Run focused tests together**

Build and run `gameplay_ruleset_policy_tests`, `replay_file_store_tests`, `profile_archive_tests`, `beatoraja_replay_codec_tests`, `beatoraja_replay_path_tests`, and `replay_repository_tests`.

- [ ] **Step 3: Run full desktop verification**

Run: `cmake --build cmake-build-debug --target main -j 6 && ctest --test-dir cmake-build-debug --output-on-failure`

Expected: all tests pass.

- [ ] **Step 4: Run iOS compile verification**

Run: `scripts/ios_firebase_deploy.sh --build-only`

Expected: `BUILD SUCCEEDED`; no upload occurs.

- [ ] **Step 5: Commit and push**

Commit the review fixes, push `feature/file-based-replays`, then report which review threads are addressed. Do not reply to or resolve GitHub threads unless the user explicitly authorizes those GitHub writes.
