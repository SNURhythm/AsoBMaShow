# PR #82 Ninth Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the three active PR #82 review findings without changing review-thread state.

**Architecture:** Preserve Beatoraja DP FLIP as explicit raw replay setup and apply it to the parsed chart before per-player modifiers. Harden chart result/replay identity with long-note mode, and share the replay temporary filename recognizer so export can ignore only files owned by replay finalization.

**Tech Stack:** C++20, nlohmann JSON, SQLite, libarchive, CMake/CTest, Xcode iOS build

## Global Constraints

- Existing Aso extension schema-version-2 BRD files must remain readable.
- Support Beatoraja `doubleoption` values 0 (normal) and 1 (FLIP); reject BATTLE values 2 and 3.
- FLIP applies before both players' play-option modifiers.
- Unknown replay-directory entries continue to fail profile export.
- Do not reply to or resolve GitHub review threads.
- Use test-first red-green cycles for every production change.

---

### Task 1: Beatoraja DP FLIP fidelity

**Files:**
- Modify: `src/replay/ReplayPlaybackData.h`
- Modify: `src/replay/BeatorajaReplayCodec.cpp`
- Modify: `src/PlayOptionUtils.h`
- Test: `tests/beatoraja_replay_codec_tests.cpp`
- Test: `tests/result_recall_builder_tests.cpp`

**Interfaces:**
- Produces: `replay::DoublePlayOption::{Normal, Flip}` and `ChartPlaybackSetup::doublePlayOption`
- Consumes: the existing parser lane-assignment modifier and replay chart preparation pipeline

- [ ] **Step 1: Write codec tests for stock FLIP, BATTLE rejection, and extension round-trip**

Patch stock fixture JSON in memory so `doubleoption` is 1, decode with expected key mode 14, and assert:

```cpp
expectEqual(decoded.chart->setup.doublePlayOption,
            replay::DoublePlayOption::Flip,
            "stock FLIP maps exactly");
```

Set the value to 2 and assert decoding fails with an unsupported double-play option diagnostic. Set `extensionReplay().setup.doublePlayOption` to `Flip`, encode/decode it, and assert the setup round-trips and outer JSON contains `doubleoption == 1`.

- [ ] **Step 2: Write a replay-preparation test proving FLIP swaps DP chart sides**

Create a temporary 14K BMS containing distinct WAV IDs on the first 1P and 2P key and scratch lanes. Parse it once for identity, set:

```cpp
playback.setup.doublePlayOption = replay::DoublePlayOption::Flip;
```

Prepare the replay chart and assert the distinct WAV IDs have exchanged player sides for both keys and scratches.

- [ ] **Step 3: Run the focused tests and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target beatoraja_replay_codec_tests result_recall_builder_tests -j 6
ctest --test-dir cmake-build-debug -R '^(beatoraja_replay_codec_tests|result_recall_builder_tests)$' --output-on-failure
```

Expected: compilation or assertions fail because `DoublePlayOption` and FLIP application do not exist.

- [ ] **Step 4: Add the setup enum and codec mapping**

Add:

```cpp
enum class DoublePlayOption : std::uint8_t { Normal = 0, Flip = 1 };
```

and a defaulted `doublePlayOption` member to `ChartPlaybackSetup`. Validate that FLIP is used only for key modes 10 and 14. Read stock `doubleoption` with a default of zero, reject values outside 0 and 1, encode the stock field from setup, and include `doublePlayOption` in the Aso setup extension. Decode the extension member only when present so older schema-version-2 files inherit the stock field.

- [ ] **Step 5: Apply FLIP before per-player replay modifiers**

Build a whole-chart assignment in DP lane order:

```cpp
// 14K: destinations L123456789ABCDER read sources R89ABCDE1234567L
const std::string option = "ASSIGN:" + flippedNotation;
```

Apply it through `applyPlayOptionModifier` before `playOption` and `playOption2`. Return false for a FLIP request on a parsed chart that is not supported 10K/14K DP.

- [ ] **Step 6: Run the focused tests and verify GREEN**

Run the commands from Step 3. Expected: both targets build and both tests pass.

- [ ] **Step 7: Commit Task 1**

```bash
git add src/replay/ReplayPlaybackData.h src/replay/BeatorajaReplayCodec.cpp src/PlayOptionUtils.h tests/beatoraja_replay_codec_tests.cpp tests/result_recall_builder_tests.cpp
git commit -m "fix: honor beatoraja double-play flip"
```

### Task 2: Chart replay long-note identity

**Files:**
- Modify: `src/repositories/ReplayRepositoryResultRecords.cpp`
- Test: `tests/replay_repository_v11_tests.cpp`

**Interfaces:**
- Consumes: `ReplayRepository::loadChartReplayPlayback(int)`
- Produces: `IntegrityConflict` when BRD and result long-note modes differ

- [ ] **Step 1: Write the failing repository regression test**

Stage a valid chart result whose `score.longNoteMode` is 1 and a valid finalized BRD whose `setup.longNoteMode` is 2. Assert:

```cpp
const auto loaded = repository.loadChartReplayPlayback(resultId);
expect(loaded.status ==
           ChartReplayPlaybackReadOutcome::Status::IntegrityConflict,
       "chart replay rejects a long-note mode mismatch");
```

- [ ] **Step 2: Run the repository test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target replay_repository_v11_tests -j 6
ctest --test-dir cmake-build-debug -R '^replay_repository_v11_tests$' --output-on-failure
```

Expected: the mismatch is currently reported as loaded.

- [ ] **Step 3: Add long-note mode to chart replay identity validation**

Change the existing comparison to include:

```cpp
decoded.chart->setup.longNoteMode != result.score.longNoteMode
```

Keep the existing identity-conflict status and diagnostic.

- [ ] **Step 4: Run the repository test and verify GREEN**

Run the commands from Step 2. Expected: pass.

- [ ] **Step 5: Commit Task 2**

```bash
git add src/repositories/ReplayRepositoryResultRecords.cpp tests/replay_repository_v11_tests.cpp
git commit -m "fix: validate chart replay long-note mode"
```

### Task 3: Ignore replay finalization temporaries during export

**Files:**
- Modify: `src/replay/ReplayFileStore.h`
- Modify: `src/replay/ReplayFileStore.cpp`
- Modify: `src/ProfileArchive.cpp`
- Test: `tests/profile_archive_tests.cpp`
- Test: `tests/replay_repository_v11_tests.cpp`

**Interfaces:**
- Produces: `replay::isPrivateReplayTemporaryFilename(std::string_view)`
- Consumes: replay finalization filename grammar `.<name>.brd.<token>.tmp`

- [ ] **Step 1: Write the failing export regression test**

Create a recent temporary alongside the valid replay:

```cpp
const auto temporary = source.replayDirectory /
    ("." + std::string(kReplayFilename) + ".attempt_token.tmp");
writeFile(temporary, "partial replay");
```

Export, assert success, assert the archive has exactly `kExpectedMembers`, and assert the source temporary still exists.

- [ ] **Step 2: Run the profile archive test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target profile_archive_tests -j 6
ctest --test-dir cmake-build-debug -R '^profile_archive_tests$' --output-on-failure
```

Expected: export fails while attempting to stage the non-`.brd` entry.

- [ ] **Step 3: Extract and reuse the temporary recognizer**

Declare a namespace-level helper in `ReplayFileStore.h`. Implement it in
`ReplayFileStore.cpp` with the same private form used by `temporaryPathFor`,
including a nonempty safe token. Replace the cleanup method's inline filename
predicate with the helper.

- [ ] **Step 4: Skip only recognized replay temporaries during export**

Include `replay/ReplayFileStore.h` in `ProfileArchive.cpp` and add before
staging:

```cpp
if (replay::isPrivateReplayTemporaryFilename(
        replayFile.filename().string())) {
  continue;
}
```

Do not remove the source temporary and do not relax `isReplayMember` for any
other entry.

- [ ] **Step 5: Extend cleanup coverage for near-miss names**

In `replay_repository_v11_tests`, retain an unrelated `keep.tmp` and a
malformed private-looking name while confirming a matching stale temporary
is cleaned. This protects the shared recognizer from becoming too broad.

- [ ] **Step 6: Run focused tests and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target profile_archive_tests replay_repository_v11_tests -j 6
ctest --test-dir cmake-build-debug -R '^(profile_archive_tests|replay_repository_v11_tests)$' --output-on-failure
```

Expected: both tests pass.

- [ ] **Step 7: Commit Task 3**

```bash
git add src/replay/ReplayFileStore.h src/replay/ReplayFileStore.cpp src/ProfileArchive.cpp tests/profile_archive_tests.cpp tests/replay_repository_v11_tests.cpp
git commit -m "fix: ignore private replay export temporaries"
```

### Task 4: Branch-wide verification and push

**Files:**
- Verify: all changed source, tests, design, and plan files

**Interfaces:**
- Produces: a verified PR branch at the same SHA locally and on `origin`

- [ ] **Step 1: Run formatting and worktree checks**

```bash
git diff --check
git status --short
```

- [ ] **Step 2: Build desktop and run the complete test suite**

```bash
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6
```

Expected: build exit 0 and zero failed tests.

- [ ] **Step 3: Run the iOS build-only verification**

```bash
scripts/ios_firebase_deploy.sh --build-only
```

Expected: `** BUILD SUCCEEDED **`; no upload occurs.

- [ ] **Step 4: Confirm the final diff and commit history**

```bash
git status --short
git diff --check
git log --oneline -10
```

- [ ] **Step 5: Push and verify PR #82 head**

```bash
git push origin feature/file-based-replays
git rev-parse HEAD
git rev-parse origin/feature/file-based-replays
gh pr view 82 --json url,state,headRefOid,statusCheckRollup
```

Expected: local, remote branch, and PR head SHAs match. Leave review threads
unchanged.
