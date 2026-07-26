# PR #82 Fifth Review Fixes Design

## Scope

Address the three actionable comments submitted against commit `0ee381ac`:

1. Show the Replay action for a final course result when either raw
   `CourseReplayPlaybackData` or legacy `JudgedCoursePlaybackData` is present.
2. Preserve combo and maximum-combo state while sequentially materializing raw
   course replay stages for video export.
3. Re-run the replay directory durability barrier before accepting an
   identical final BRD during a finalize retry.

Beatoraja slot relocation and slot-selection UI remain deferred. This work does
not alter the BRD format, replay path grammar, database schema, or GitHub review
thread state.

## Verified Root Causes

### Course Replay Action

`ResultScene::addCourseButtons()` gates the Replay button exclusively on
`CoursePlaySession::courseReplayData`. The launcher,
`ResultScene::startCourseReplay()`, already supports both
`courseReplayPlaybackData` and `courseReplayData`. Newly saved file-backed
course replays populate only the raw representation, so the presentation gate
and execution gate disagree.

### Course Video Combo Carry

`ReplayVideoExporter` materializes each raw stage with a fresh
`GameplaySimulation`. `GameplaySimulation` already supports `carriedCombo` and
`carriedMaxCombo`, but `replay::materializeReplay()` does not expose them and
therefore initializes every stage at zero. The stage setup already captures
the correct starting gauge, so only combo state needs an additional seed.

### Finalize Retry Durability

After a successful no-replace rename, `ReplayFileStore::finalize()` explicitly
syncs the replay directory. If that sync reports failure, a retry sees the
installed final file and returns through `validateFinal()` without attempting
the directory sync again. The same bypass exists when a concurrent writer wins
the no-replace race. Byte and identity validation cannot substitute for the
directory-entry durability barrier.

## Considered Approaches

### 1. Targeted boundary fixes (selected)

- Add a pure course replay action predicate shared by the UI and focused tests.
- Add an optional combo seed to `materializeReplay()` and have the course video
  loop update it from each materialized attempt.
- Route existing-identical finalize paths through a helper that syncs the
  parent directory before validating the final file.

This keeps all default single-chart materialization behavior unchanged and
fixes durability at the storage boundary.

### 2. Reconstruct course video from persisted judged stage results

The exporter could overwrite materialized combo annotations using database
stage results. Persisted results do not contain every per-note combo value, so
this cannot faithfully repair the video HUD.

### 3. Add course carry state to the BRD extension

The codec could store a per-stage starting combo. That changes the on-disk
extension for state the exporter can deterministically derive from the
preceding stage, increasing compatibility and migration surface without need.

## Design

### Replay Action Availability

Add `result_scene_detail::courseReplayActionAvailable(const
CoursePlaySession&) noexcept` next to the course persistence receipt helper.
It returns `session.hasCourseReplayStage(0)`, which already recognizes either
raw or legacy replay storage and requires a non-empty first stage.
`ResultScene::addCourseButtons()` uses this predicate. The launcher's existing
defensive checks remain in place.

### Materialization Seed

Add `replay::ReplayMaterializationSeed` with nonnegative `carriedCombo` and
`carriedMaxCombo` fields. `materializeReplay()` accepts it as a defaulted final
parameter and copies the values into `GameplayAttemptOptions`. Existing callers
therefore retain zero-initialized chart behavior.

During raw course video adaptation, maintain one seed outside the stage loop.
After each successful materialization, set the next seed from
`MaterializedReplay::attempt.combo` and `.maxCombo`. A combo break naturally
sets the carried combo to zero while retaining the course maximum. Legacy
stages continue through their isolated adapter; because a raw/legacy mixture
does not provide a trustworthy raw simulation boundary, the seed is updated
only from successfully materialized raw stages and the existing legacy path is
otherwise unchanged.

The current codec and replay schema need no new field because the state is
derived in stage order.

### Existing-File Finalization

Create a private helper in `ReplayFileStore.cpp` that:

1. honors the existing `directory-sync` fault-injection point,
2. calls `atomic_file::syncDirectory(finalPath.parent_path(), diagnostic)`,
3. calls `validateFinal(...)` only after the sync succeeds.

Use it for the early existing-file branch and the no-replace
`DestinationExists` branch. The newly renamed branch keeps its explicit sync
and validation flow. This may issue an extra harmless directory sync for an
already durable idempotent retry, which is the conservative behavior required
when the caller cannot know whether the previous sync completed.

## Error Handling

- Missing replay data continues to hide the Replay action and the launcher
  continues to fail closed.
- Negative combo seeds are clamped to zero, and carried maximum is at least the
  carried combo, matching `RhythmState` invariants.
- An existing replay is not reported finalized when directory sync fails, even
  when its bytes and decoded identity are valid.
- No final BRD is deleted after a post-rename sync failure; a later retry can
  safely re-run the barrier.

## Test Strategy

1. Extend `remote_result_scene_tests` with raw, legacy, and empty session
   action-availability cases, observing failure before the UI predicate exists.
2. Extend `replay_playback_driver_tests` with sequential one-note stage
   materialization. Stage two must start from stage one's final combo and
   maximum, observing failure before the seed is accepted.
3. Strengthen `replay_file_store_tests` so a finalize that installs the file
   and fails directory sync is retried with the same injected sync failure.
   The retry must still fail, then succeed when the barrier succeeds.
4. Run the focused targets, the complete desktop test suite, the desktop app
   build, and `scripts/ios_firebase_deploy.sh --build-only`.

## Acceptance Criteria

- A final result with only raw course replay data exposes Replay.
- Sequential raw course video stages preserve combo and course max combo.
- Identical existing BRDs cannot be accepted until their parent directory sync
  succeeds.
- All existing chart replay, legacy course replay, and file-store behavior
  remains green.
- Deferred slot relocation code and UI are not changed.
