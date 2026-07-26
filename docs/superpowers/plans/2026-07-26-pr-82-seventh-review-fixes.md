# PR 82 Seventh Review Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Correct the four current-head review defects in touch replay capture, raw replay ruleset gating, and occupied Beatoraja slot replacement without corrupting or discarding an existing replay reference.

**Architecture:** Move legacy touch-to-replay translation into a small pure helper so 48K player/lane mapping and reversal timestamp grouping have direct tests, then make `RhythmInputHandler` consume that helper. Preserve the recorded ruleset revision in `StartOptions` even when unsupported so the existing gameplay policy gate rejects it. Make occupied-slot copying repository-aware: copy the displaced replay to a newly reserved history path, atomically retarget its existing `replay_files` row to that path, then overwrite the now-unreferenced visible slot.

**Tech Stack:** C++23, CMake/CTest, SQLite, existing durable atomic-file helpers, Beatoraja-compatible BRD files.

## Global Constraints

- Keep replay events exclusively in BRD files; do not add event payloads back to SQLite.
- Preserve chart/course result facts and replay-file references independently from IR provenance.
- Keep Beatoraja's `replay/<stem>[_history].brd` layout.
- Do not reply to or resolve GitHub review threads without separate authorization.
- Do not reintroduce legacy replay reconstruction.
- Use test-first red/green cycles for every production behavior change.
- Use `scripts/ios_firebase_deploy.sh --build-only` for iOS verification; do not upload a build.

---

### Task 1: Correct legacy touch replay mapping and reversal grouping

**Files:**
- Create: `src/input/TouchReplayTransition.h`
- Modify: `src/input/RhythmInputHandler.h`
- Modify: `src/input/RhythmInputHandler.cpp`
- Modify: `tests/logical_gameplay_input_tests.cpp`

**Interfaces:**
- Produces: `touch_replay::transition(int keyMode, int physicalLane, bool pressed, std::optional<int> scratchDirection, std::int64_t steadyTimestampMicros) -> LogicalGameplayInputAdapter::AppliedTransition`.
- Produces: `touch_replay::scratchReversal(int keyMode, int physicalLane, int previousDirection, int nextDirection, std::int64_t steadyTimestampMicros) -> std::array<LogicalGameplayInputAdapter::AppliedTransition, 2>`.
- Consumes: existing `LogicalGameplayInputAdapter::AppliedTransition`, `input::InputScope`, and `replay::LogicalControl` types.

- [ ] **Step 1: Write failing 48K and reversal tests**

Add cases to `tests/logical_gameplay_input_tests.cpp` which use literal expectations:

```cpp
const auto first48k = touch_replay::transition(48, 26, true, std::nullopt, 101);
require(first48k.control == replay::LogicalControl{
                               .kind = replay::LogicalControlKind::Lane,
                               .player = 2,
                               .lane = 0},
        "48K touch lane 26 records as player-two lane zero");
const auto last48k = touch_replay::transition(48, 51, true, std::nullopt, 102);
require(last48k.control.player == 2 && last48k.control.lane == 25,
        "48K touch lane 51 remains inside the replay lane range");

const auto reversal =
    touch_replay::scratchReversal(7, 7, 1, -1, 123'456);
require(!reversal[0].pressed && reversal[1].pressed &&
            reversal[0].source.steadyTimestampMicros == 123'456 &&
            reversal[1].source.steadyTimestampMicros == 123'456 &&
            reversal[0].control.kind ==
                replay::LogicalControlKind::ScratchClockwise &&
            reversal[1].control.kind ==
                replay::LogicalControlKind::ScratchCounterClockwise,
        "touch reversal emits one timestamped release/press pair");
```

- [ ] **Step 2: Run the focused test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target logical_gameplay_input_tests -j 6
```

Expected: build/test setup fails because `touch_replay::transition` and `touch_replay::scratchReversal` do not exist yet.

- [ ] **Step 3: Implement the pure transition helper**

Create `src/input/TouchReplayTransition.h` with inline functions. Player two is selected for `(keyMode == 10 || keyMode == 14) && physicalLane >= 8` or `keyMode == 48 && physicalLane >= 26`; its local-lane offset is respectively 8 or 26. Scratch controls always use logical lane `-1`. `scratchReversal` must build both elements from one caller-supplied timestamp.

- [ ] **Step 4: Route `RhythmInputHandler` through the helper**

Change `notifyTouchLaneApplied` to accept an explicit steady timestamp and dispatch `touch_replay::transition`. In `handleScratchMove`, sample `steadyNowMicros()` exactly once after detecting a direction change. When a prior scratch direction is active, dispatch the two results from `touch_replay::scratchReversal`; otherwise dispatch the initial directed press. Pass one sampled timestamp for ordinary touch press/release notifications as well.

- [ ] **Step 5: Run the focused test to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target logical_gameplay_input_tests -j 6
ctest --test-dir cmake-build-debug -R '^foundation_input_gameplay$' --output-on-failure
```

Expected: build succeeds and the registered test passes.

- [ ] **Step 6: Commit the touch fix**

```bash
git add src/input/TouchReplayTransition.h src/input/RhythmInputHandler.h src/input/RhythmInputHandler.cpp tests/logical_gameplay_input_tests.cpp
git commit -m "fix: preserve touch replay controls"
```

### Task 2: Reject unsupported raw replay ruleset revisions

**Files:**
- Modify: `tests/gameplay_playback_startup_tests.cpp`
- Modify: `src/scene/play/GamePlayStartOptions.h`

**Interfaces:**
- Consumes: `ChartPlaybackSetup::playbackRulesetId` and `playbackRulesetRevision`.
- Produces: `StartOptions::requiredRulesetDescriptor` containing the recorded revision for every known ruleset ID, allowing `buildGameplayRulesetPolicyAtPlayStart` to return `UnsupportedRuleset` when the revision is unavailable.

- [ ] **Step 1: Write the failing revision-preservation test**

After the supported raw replay assertions, apply a Beatoraja playback whose revision is one below `RulesetDescriptor::For(GameplayRuleset::Beatoraja).version` and assert:

```cpp
auto unsupported = std::make_shared<replay::ReplayPlaybackData>();
unsupported->setup.playbackRulesetId = "beatoraja";
unsupported->setup.playbackRulesetRevision = 1;
StartOptions unsupportedOptions;
applyReplayPlaybackToStartOptions(unsupportedOptions, unsupported);
const auto required = unsupportedOptions.requiredRulesetDescriptor;
if (!expect(required.has_value() && required->id == "beatoraja" &&
                required->version == 1 &&
                !isSupportedRulesetDescriptor(*required),
            "raw replay preserves an unsupported recorded ruleset revision")) {
  return 1;
}
```

- [ ] **Step 2: Run the focused test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_playback_startup_tests -j 6
ctest --test-dir cmake-build-debug -R '^gameplay_playback_startup_tests$' --output-on-failure
```

Expected: FAIL because the mismatch currently leaves `requiredRulesetDescriptor` empty.

- [ ] **Step 3: Preserve the recorded revision**

In `applyReplayPlaybackToStartOptions`, always start from `RulesetDescriptor::For(*ruleset)`, replace its `version` with `setup.playbackRulesetRevision`, and assign it to `options.requiredRulesetDescriptor`. Keep selecting the known `GameplayRuleset`; the existing policy builder will accept the exact supported descriptor and reject any modified revision.

- [ ] **Step 4: Run the focused test to verify GREEN**

Run the same build and CTest command. Expected: PASS.

- [ ] **Step 5: Commit the ruleset fix**

```bash
git add tests/gameplay_playback_startup_tests.cpp src/scene/play/GamePlayStartOptions.h
git commit -m "fix: reject unsupported replay rulesets"
```

### Task 3: Preserve displaced replay references during occupied-slot replacement

**Files:**
- Modify: `src/repositories/ReplayRepository.h`
- Modify: `src/repositories/ReplayRepositoryInternal.h`
- Modify: `src/repositories/ReplayRepositoryResultRecords.cpp`
- Modify: `src/replay/ReplayFileStore.h`
- Modify: `src/replay/ReplayFileStore.cpp`
- Modify: `src/replay/ReplayFileActionService.cpp`
- Modify: `tests/result_persistence_v11_integration_tests.cpp`

**Interfaces:**
- Produces: `ReplayFileReferenceLookupOutcome ReplayRepository::loadReplayFileReference(std::string_view stem, std::int64_t historyIndex)`.
- Produces: `ReplayFileRelocationOutcome ReplayRepository::relocateReplayFileReference(const ReplayFileReference &expected, const ReplayFileReservation &destination)`.
- Produces: `bool ReplayRepository::discardReplayFileReservation(const ReplayFileReservation &reservation, std::string &diagnostic)` for exact-match cleanup after a failed relocation copy.
- Produces: `bool ReplayFileStore::copyToReservedReplayPath(const ReplayFileMetadata &source, const ReplayPathIdentity &destination, std::string &diagnostic)`; it must never replace different destination bytes.
- Consumes: the existing `reserveReplayFile`, `pathForStem`, durable no-replace rename, and `ProfileDatabaseActivity` write serialization.

- [ ] **Step 1: Write the failing end-to-end occupied-slot test**

In `tests/result_persistence_v11_integration_tests.cpp`, persist five attempts for the same chart SHA using canonical distinct attempt UUIDs and distinct `playedAtUnixMillis` values. Save the original bytes and result IDs for history indexes 1 and 4, then invoke:

```cpp
ReplayFileActionService actions(environment.replayRepository,
                                environment.fileStore);
const auto copied = actions.copyToBeatorajaSlot(
    {.kind = ReplayFileReference::RecordKind::ChartResult,
     .resultId = resultIds[4]},
    1);
```

Assert literal behavior:

```cpp
expect(copied.changed && copied.availability == ReplayAvailability::Available,
       "occupied visible slot replacement succeeds");
const auto displaced =
    environment.replayRepository.loadChartResult(resultIds[1]);
const auto selected =
    environment.replayRepository.loadChartResult(resultIds[4]);
expect(displaced.record && displaced.record->replayFile &&
           displaced.record->replayFile->historyIndex == 5,
       "displaced replay reference moves to the next history path");
expect(selected.record && selected.record->replayFile &&
           selected.record->replayFile->historyIndex == 4,
       "selected result keeps its original replay reference");
expect(readBytes(root / "replay" / (stem + "_1.brd")) == selectedBytes,
       "visible slot contains the selected replay bytes");
expect(readBytes(root / "replay" / (stem + "_5.brd")) == displacedBytes,
       "displaced replay bytes survive at the relocated reference");
expect(actions.inspect(displacedId).availability ==
           ReplayAvailability::Available &&
           actions.inspect(selectedId).availability ==
           ReplayAvailability::Available,
       "both result replay references remain valid");
expect(scalar(environment.replayDatabase,
              "SELECT count(*) FROM replay_files") == 5 &&
           scalar(environment.replayDatabase,
                  "SELECT count(*) FROM replay_file_reservations") == 0,
       "relocation consumes its reservation without dropping references");
```

- [ ] **Step 2: Run the integration test to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target result_persistence_integration_tests -j 6
ctest --test-dir cmake-build-debug -R '^result_persistence_integration_tests$' --output-on-failure
```

Expected: FAIL because history index 1 still owns the overwritten path and inspection reports it corrupt.

- [ ] **Step 3: Add repository lookup and atomic relocation**

Implement a canonical `(stem, historyIndex)` lookup that returns the complete validated `ReplayFileReference`. Implement relocation inside `BEGIN IMMEDIATE`: verify the stored row still exactly matches `expected`, verify the reservation exactly matches `destination`, update only `history_index` and `relative_path`, delete the consumed reservation, and commit. Return `IntegrityConflict` rather than changing a row when either snapshot changed. Implement exact-match reservation discard in its own immediate transaction for failure cleanup.

- [ ] **Step 4: Add durable no-replace copying to a reserved path**

Implement `copyToReservedReplayPath` by revalidating source bytes, verifying `destination == pathForStem(destination.stem, destination.historyIndex)`, writing a private temporary file, durably renaming with no replacement, and syncing the replay directory. Treat an already-present byte-identical destination as an idempotent retry; reject different or unsafe destination contents.

- [ ] **Step 5: Make the action service repository-aware**

Before overwriting a different visible-slot file, load any database owner of `(stem, slot)`. If no owner exists, keep the existing direct durable replacement. If the owner has identical content metadata, return through the idempotent copy path. Otherwise:

1. Verify the displaced owner's current file is available.
2. Reserve the next history path with a fresh `uuid::generateV4()` token.
3. Copy the displaced bytes to that reserved path without replacement.
4. Atomically relocate the displaced `replay_files` row and consume the reservation.
5. Only after relocation succeeds, copy the selected bytes into the now-unreferenced visible slot.

If steps 2–4 fail, remove any newly copied relocation file and discard the exact reservation. Never overwrite a slot while a different database row still owns its path.

- [ ] **Step 6: Run the focused integration test to verify GREEN**

Run the same build and CTest command. Expected: PASS with both references available and no outstanding reservation.

- [ ] **Step 7: Run repository and file-store regression tests**

Run:

```bash
cmake --build cmake-build-debug --target replay_file_store_tests replay_repository_tests result_persistence_integration_tests -j 6
ctest --test-dir cmake-build-debug -R '^(replay_file_store_tests|replay_repository_tests|result_persistence_integration_tests)$' --output-on-failure
```

Expected: all three registered tests pass.

- [ ] **Step 8: Commit the slot reconciliation fix**

```bash
git add src/repositories/ReplayRepository.h src/repositories/ReplayRepositoryInternal.h src/repositories/ReplayRepositoryResultRecords.cpp src/replay/ReplayFileStore.h src/replay/ReplayFileStore.cpp src/replay/ReplayFileActionService.cpp tests/result_persistence_v11_integration_tests.cpp
git commit -m "fix: preserve displaced replay references"
```

### Task 4: Verify and publish the existing PR branch

**Files:**
- Verify only; no additional production files expected.

**Interfaces:**
- Consumes: all three preceding task commits.
- Produces: a clean pushed `feature/file-based-replays` head for PR #82.

- [ ] **Step 1: Run focused builds and tests**

```bash
cmake --build cmake-build-debug --target logical_gameplay_input_tests gameplay_playback_startup_tests result_persistence_integration_tests replay_file_store_tests replay_repository_tests main -j 6
ctest --test-dir cmake-build-debug -R '^(foundation_input_gameplay|gameplay_playback_startup_tests|result_persistence_integration_tests|replay_file_store_tests|replay_repository_tests)$' --output-on-failure
```

- [ ] **Step 2: Run the complete desktop suite**

```bash
ctest --test-dir cmake-build-debug -j 6 --output-on-failure
```

Expected: every registered test passes.

- [ ] **Step 3: Run the iOS build-only workflow**

```bash
scripts/ios_firebase_deploy.sh --build-only
```

Expected: `** BUILD SUCCEEDED **`; no Firebase upload occurs.

- [ ] **Step 4: Audit and push**

```bash
git diff --check
git status -sb
git log --oneline origin/feature/file-based-replays..HEAD
git push origin feature/file-based-replays
gh pr view 82 --json url,headRefOid,state
```

Expected: the tree is clean and local, remote, and PR head OIDs match. Do not reply to or resolve review threads.
