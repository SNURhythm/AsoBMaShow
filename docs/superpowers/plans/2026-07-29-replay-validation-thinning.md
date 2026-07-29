# Replay Validation Thinning Implementation Plan

**Goal:** Stop valid local plays from losing their BRD attachment by removing
premature and duplicate validation while retaining one safety, identity, and
ownership authority per fact.

**Architecture:** Normalize local observer streams once before capture is
bound to a result. Let the BRD codec own structural validation, agreement own
result identity/setup, the file store own bytes and paths, and persistence own
attempt association. Consumers trust successful codec validation and compare
only the facts required to bind the replay to the requested history record.

---

## Task 1: Retain realtime input capture

1. Add failing tests for input after the cursor snapshot and interleaved input
   timestamps.
2. Expand an observed completion bound to accepted input and remove the
   realtime worker's source-order rejection.
3. Run replay input and realtime worker tests.
4. Commit independently. (Completed by `aaf7c3d8`.)

## Task 2: Normalize every local observer stream

**Files:**

- Modify `src/replay/ReplayInputRecorder.h`
- Modify `src/replay/ReplayInputRecorder.cpp`
- Modify `src/scene/play/GamePlayScene.cpp`
- Modify `tests/replay_input_recorder_tests.cpp`

1. Add a failing test with interleaved input, touch, and lane-cover timestamps.
2. Add one local-capture normalizer that applies limits, stable ordering, input
   state normalization, and the shared derived completion bound.
3. Replace the input-only completion call site with the shared normalizer.
4. Run replay input, chart capture, course capture, and realtime worker tests.
5. Commit independently.

## Task 3: Remove parsed-duration rejection

**Files:**

- Modify `tests/chart_replay_context_tests.cpp`
- Modify `tests/course_replay_context_tests.cpp`
- Modify `src/replay/ChartReplayContext.cpp`
- Modify `src/replay/CourseReplayContext.cpp`

1. Add failing context tests showing that a valid decoded BRD remains ready
   when an optional parsed duration estimate differs.
2. Stop passing parser estimates as decoder requirements and remove exact
   decoded-bound comparisons. Keep invalid supplied values as invalid requests.
3. Run chart/course codec and context tests.
4. Commit independently.

## Task 4: Give structural validation one owner

**Files:**

- Modify chart/course agreement, capture, persistence, context, and codec files
  as required.
- Modify focused agreement/capture/context tests.

1. Keep existing identity/setup mismatch tests green.
2. Make agreement compare only valid result shape and binding facts; remove its
   playback-envelope validation.
3. Remove post-decode structural revalidation from contexts. Keep codec decode
   as the untrusted-file structural authority.
4. Ensure local capture is normalized and binding-checked, while encode owns
   the final structural check before bytes are installed.
5. Run all replay agreement, capture, persistence, codec, and context tests.
6. Commit independently.

## Task 5: Audit, verify, publish

1. Diff against `origin/develop` and inventory all remaining replay rejections.
   Confirm every rejection maps to safety, compatibility, identity, ownership,
   or atomicity and has one runtime authority.
2. Run all focused contract, integration, migration, and file-state tests.
3. Run the full configured CTest suite.
4. Build desktop `main` and run the iOS build-only verification.
5. Commit any audit corrections, push the branch, monitor PR checks, and allow
   the already-authorized deployment workflow to complete.

