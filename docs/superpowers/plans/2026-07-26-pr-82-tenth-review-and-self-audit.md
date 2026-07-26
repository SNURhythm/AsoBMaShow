# PR 82 Tenth Review and Self-Audit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the five newest PR 82 replay findings and iteratively audit the complete branch diff until no further concrete correctness issue remains.

**Architecture:** Keep replay setup in AsoBMaShow's application domain and centralize Beatoraja projection at stock JSON and filename boundaries. Make recorder failure and timestamp overflow explicit fail-closed outcomes, then use independent read-only review passes to examine the full cross-cutting replay change while the primary session validates and implements all fixes.

**Tech Stack:** C++20, nlohmann JSON, gzip/Base64URL BRD codec, SQLite repositories, CMake/CTest, Xcode iOS build script.

## Global Constraints

- Preserve `ReplayPlaybackData::setup.longNoteMode` as application values `0/1/2/3` for no-LN/LN/CN/HCN.
- Emit Beatoraja stock LN values and path prefixes only through one shared conversion helper.
- Do not reply to or resolve GitHub review threads.
- Do not restore legacy replay reconstruction or slot relocation.
- Write a failing regression test before every behavior change.
- Run delegated audit agents read-only; the primary session validates findings and owns all edits.

---

### Task 1: Centralize Aso-to-Beatoraja long-note conversion

**Files:**

- Create: `src/replay/BeatorajaLongNoteMode.h`
- Modify: `src/replay/BeatorajaReplayCodec.cpp`
- Modify: `src/replay/BeatorajaReplayPath.cpp`
- Modify: `src/repositories/ReplayRepositoryReplayFileMigration.cpp`
- Test: `tests/beatoraja_replay_codec_tests.cpp`
- Test: `tests/beatoraja_replay_path_tests.cpp`
- Test: `tests/replay_file_migration_tests.cpp`

**Interfaces:**

- Produces: `replay::stockLongNoteMode(int) -> std::optional<int>` and `replay::applicationLongNoteMode(int) -> std::optional<int>`.
- Consumes: application modes `0..3`, Beatoraja stock modes `0..2`.

- [ ] **Step 1: Write conversion and boundary RED tests**

Add path assertions that application LN/CN/HCN modes `1/2/3` produce
`""/"C"/"H"` for undefined charts, that mode `0` is rejected for an
undefined chart, and that HCN migration remains `3`. Add codec assertions
that Aso HCN encodes stock `mode: 2`, round-trips application mode `3`, and a
stock fixture `mode: 1` decodes as application CN `2`.

- [ ] **Step 2: Run RED**

Run:

```sh
cmake --build cmake-build-debug --target beatoraja_replay_codec_tests beatoraja_replay_path_tests replay_file_migration_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(beatoraja_replay_codec_tests|beatoraja_replay_path_tests|replay_file_migration_tests)$'
```

Expected: old `0..2` assumptions fail CN/HCN projection and migration HCN.

- [ ] **Step 3: Implement the shared conversion boundary**

Create a header-only conversion with these exact mappings:

```cpp
stockLongNoteMode(0) == 0;
stockLongNoteMode(1) == 0;
stockLongNoteMode(2) == 1;
stockLongNoteMode(3) == 2;
applicationLongNoteMode(0) == 1;
applicationLongNoteMode(1) == 2;
applicationLongNoteMode(2) == 3;
```

Return `std::nullopt` outside the supported domains. Use it in codec stock
encode/decode and path prefix selection. Change setup validation and migration
clamps to accept application HCN `3`.

- [ ] **Step 4: Run GREEN and commit**

Run the Step 2 command and `git diff --check`. Commit the focused conversion
and tests as `fix: translate beatoraja long-note modes`.

---

### Task 2: Bind stock and extension replay identity

**Files:**

- Modify: `src/replay/BeatorajaReplayCodec.cpp`
- Modify: `src/replay/ReplayFileStore.h`
- Modify: `src/replay/ReplayFileStore.cpp`
- Modify: `src/ResultPersistenceCoordinator.cpp`
- Test: `tests/beatoraja_replay_codec_tests.cpp`
- Test: `tests/replay_file_store_tests.cpp`
- Test: `tests/result_persistence_coordinator_v11_tests.cpp`

**Interfaces:**

- Produces: Aso extension setup fields `chartSha256` and application `longNoteMode`.
- Changes: `ExpectedReplayIdentity` carries ordered application LN modes beside ordered SHA-256 values.

- [ ] **Step 1: Write identity mismatch RED tests**

Mutate a valid Aso BRD so stock `sha256` disagrees with the extension, then so
stock `mode` disagrees with the extension projection; both decodes must fail.
Finalize an otherwise valid BRD against an expected wrong application LN mode;
file validation must fail.

- [ ] **Step 2: Run RED**

Run:

```sh
cmake --build cmake-build-debug --target beatoraja_replay_codec_tests replay_file_store_tests result_persistence_coordinator_v11_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(beatoraja_replay_codec_tests|replay_file_store_tests|result_persistence_coordinator_v11_tests)$'
```

Expected: current decoding accepts contradictory stock identity and file
finalization checks only SHA-256.

- [ ] **Step 3: Implement exact identity consistency**

Encode and decode exact Aso `chartSha256` and application `longNoteMode`.
Preserve stock SHA-256 and stock mode before extension decode; reject unequal
SHA-256 or unequal projected mode, while retaining the existing double-play
check. Validate expected SHA/mode vector sizes and compare both fields for
every finalized chart/course stage. Populate expected modes in both
coordinator paths.

- [ ] **Step 4: Run GREEN and commit**

Run the Step 2 command and `git diff --check`. Commit as
`fix: bind stock and extension replay identity`.

---

### Task 3: Detect undefined long scratches

**Files:**

- Modify: `src/replay/ReplayPlaybackData.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Test: `tests/beatoraja_replay_path_tests.cpp`

**Interfaces:**

- Produces: `replay::hasUndefinedLongNotesForReplay(int authoredMode, int totalLongNotes, int totalBackSpinNotes) -> bool`.

- [ ] **Step 1: Write the backspin-only RED test**

Assert that authored mode `0`, zero keyboard long notes, and one backspin is
undefined; authored modes `1..3` and zero total long/backspin notes are not.

- [ ] **Step 2: Run RED**

Build and run `beatoraja_replay_path_tests`; expect the missing helper or old
keyboard-only rule to fail.

- [ ] **Step 3: Implement and wire the helper**

Return `authoredMode == 0 && (totalLongNotes > 0 || totalBackSpinNotes > 0)`
and use it when constructing recorded playback setup.

- [ ] **Step 4: Run GREEN and commit**

Run the focused path test and `git diff --check`. Commit as
`fix: detect undefined long scratches in replays`.

---

### Task 4: Fail replay capture atomically

**Files:**

- Modify: `src/replay/ReplayInputRecorder.h`
- Modify: `src/replay/ReplayInputRecorder.cpp`
- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Test: `tests/replay_input_recorder_tests.cpp`
- Test: `tests/replay_decoupling_audit_tests.cpp`

**Interfaces:**

- Changes: `ReplayInputRecorder::finish(std::string&) -> std::optional<std::vector<InputTransition>>`.
- Produces: an engaged vector for valid capture, including all-miss; `nullopt` plus the first fatal diagnostic after nonredundant rejection.

- [ ] **Step 1: Write fatal-overflow and caller-gating RED tests**

Record up to a two-transition limit, reject a third effective transition, and
assert `finish()` is `nullopt`; separately assert redundant press/release
samples do not poison a valid finish. Extend the existing source-boundary
audit so chart attempt construction is gated by the scene's raw capture
failure state.

- [ ] **Step 2: Run RED**

Run:

```sh
cmake --build cmake-build-debug --target replay_input_recorder_tests replay_decoupling_audit_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^(replay_input_recorder_tests|replay_decoupling_audit_tests)$'
```

Expected: recorder exposes the accepted prefix and the scene has no fatal
capture gate.

- [ ] **Step 3: Implement sticky fatal failure and persistence gating**

Store the first nonredundant recorder diagnostic. Duplicate presses and
unmatched releases remain ordinary false returns without poisoning finish.
Make `finish()` return `nullopt` on fatal failure. Reset and set a
`rawReplayCaptureFailed` scene member; do not build a chart persistence
attempt or append a course raw stage when it is set.

- [ ] **Step 4: Run GREEN and commit**

Run the Step 2 command and `git diff --check`. Commit as
`fix: abort truncated replay persistence`.

---

### Task 5: Check replay materialization time arithmetic

**Files:**

- Modify: `src/replay/ReplayPlaybackMaterializer.cpp`
- Test: `tests/replay_playback_driver_tests.cpp`

**Interfaces:**

- Produces: `MaterializeOutcome::Status::Invalid` when the completion-time addition would overflow.

- [ ] **Step 1: Write the `INT64_MAX` RED test**

Give a valid logical input transition timestamped
`std::numeric_limits<std::int64_t>::max()` and assert materialization returns
`Invalid` with no value.

- [ ] **Step 2: Run RED**

Build and run `replay_playback_driver_tests` under the normal test command;
expect the current unchecked addition not to return the specified invalid
outcome.

- [ ] **Step 3: Add checked completion-time arithmetic**

Validate that the nonnegative late window and final tick fit below
`INT64_MAX - baseTime`; otherwise return `Invalid` before calling
`simulation.advanceTo()`.

- [ ] **Step 4: Run GREEN and commit**

Run the focused test and `git diff --check`. Commit as
`fix: bound replay materialization time`.

---

### Task 6: Perform independent full-diff audit passes

**Files:**

- Inspect: every path in `git diff --name-status develop...HEAD`
- Modify/Test: only files tied to confirmed findings

**Interfaces:**

- Inputs: the complete branch diff, original replay spec, current PR threads.
- Output: evidence-backed findings with exact path/line, impact, and reproduction; no agent edits.

- [ ] **Step 1: Dispatch three read-only reviewers**

Assign codec/compatibility/file integrity, migration/persistence/profile
atomicity, and gameplay/result/course/IR lifecycle. Require each reviewer to
inspect current code, ignore already-fixed stale comments, and report only
concrete defects with a reproducible path.

- [ ] **Step 2: Validate every report locally**

For each report, inspect the exact current diff and trace producer → durable
representation → consumer. Reject speculative or already-fixed findings.

- [ ] **Step 3: TDD each confirmed finding**

For each confirmed behavior defect, add the smallest real regression test,
run it to observe the expected failure, apply the minimal production fix, run
the focused test green, and commit with one intent-focused message.

- [ ] **Step 4: Repeat the audit**

Re-run local passes over identity propagation, integer/size bounds,
transaction and file cleanup, migration rollback, profile import/export,
cross-platform build membership, and replay/result/IR decoupling. Repeat Step
3 until a complete pass produces no new concrete finding.

---

### Task 7: Full verification and publication

**Files:**

- Verify: complete worktree and PR branch

- [ ] **Step 1: Run formatting and desktop verification**

```sh
git diff --check develop...HEAD
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: build exit `0`; all tests pass with zero failures.

- [ ] **Step 2: Run iOS build-only verification**

```sh
scripts/ios_firebase_deploy.sh --build-only
```

Expected: compile/archive validation exits `0` without upload.

- [ ] **Step 3: Review branch state and push**

Confirm `git status --short` is clean, review `git log --oneline develop..HEAD`
and `git diff --stat develop...HEAD`, push `feature/file-based-replays`, and
verify the remote PR head SHA equals local `HEAD`.
