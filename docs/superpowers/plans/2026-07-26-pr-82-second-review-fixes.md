# PR #82 Second Review Fixes Implementation Plan

**Goal:** Address the five new replay-file review findings without re-coupling raw replay input to persisted score or IR provenance.

**Architecture:** Keep replay compatibility rules in small shared helpers, and exercise each affected boundary through focused regression tests. Legacy MD5-only rows receive a deterministic, namespaced synthetic SHA-256 solely because the v11 file layout requires a SHA stem; recalled charts accept that legacy identity only when their real MD5 still matches. Runtime replay materialization continues to derive judgements from raw input, but now restores every recorded gameplay rule that can change those judgements.

**Tech Stack:** C++20, SQLite, CMake/CTest, Beatoraja replay codec/path layout, Xcode iOS build script.

---

### Task 1: Preserve valid MD5-only legacy replays during atomic migration

**Files:**
- Create: `src/replay/LegacyReplayIdentity.h`
- Modify: `src/repositories/ReplayRepositoryReplayFileMigration.cpp`
- Test: `tests/replay_file_migration_tests.cpp`

1. Add a v10 fixture regression that clears the chart SHA-256 while retaining its canonical MD5, migrates it, and expects a committed v11 result plus a readable `.brd` file.
2. Run `replay_file_migration_tests` and confirm the test fails because the empty SHA cannot produce a chart stem or valid persisted result.
3. Add a deterministic synthetic SHA-256 helper using the domain-separated input `asobmashow:legacy-md5:v1:<md5>` and reject malformed MD5 values.
4. Apply the synthetic SHA to the legacy chart before pending-result validation and replay-path construction; preserve the original MD5 in the result and replay setup.
5. Re-run the focused migration test to green.

### Task 2: Reject recalled charts whose current content no longer matches the stored identity

**Files:**
- Modify: `src/replay/LegacyReplayIdentity.h`
- Modify: `src/ResultRecallBuilder.cpp`
- Test: `tests/result_recall_builder_tests.cpp`

1. Add chart and course regressions whose loader returns a different chart hash, and assert recall fails before stored metadata can overwrite the parsed identity.
2. Add a compatibility regression proving a synthetic MD5-only migration identity accepts a parsed chart with the same MD5 and its real SHA-256.
3. Run `result_recall_builder_tests` and observe the replacement-chart cases fail.
4. Compare parsed metadata against stored hashes before calling `applyStoredMeta`; require normal SHA equality, or matching MD5 only when the stored SHA is the exact deterministic legacy fallback for that MD5.
5. Re-run the focused recall tests to green.

### Task 3: Restore recorded candidate-selection behavior while materializing raw replay input

**Files:**
- Create: `src/scene/play/GameplayCandidateSelection.h`
- Modify: `src/scene/play/GamePlayStartOptions.h`
- Modify: `src/replay/ReplayPlaybackMaterializer.cpp`
- Test: `tests/replay_playback_driver_tests.cpp`

1. Add a regression with two simultaneously hittable notes where Combo selection chooses the later note but Lowest chooses the earlier note.
2. Run `replay_playback_driver_tests` and confirm raw materialization incorrectly selects the earlier note.
3. Move the existing candidate-selection-to-note-priority mapping into a focused shared header and use it from both gameplay startup and replay materialization.
4. Re-run the focused driver/materializer test to green.

### Task 4: Apply saved course constraints before raw replay video materialization

**Files:**
- Modify: `src/scene/play/GamePlayStartOptions.h`
- Modify: `src/ReplayVideoExporter.cpp`
- Test: `tests/gameplay_ruleset_policy_tests.cpp`

1. Add a regression that applies saved course replay setup with `NO GOOD`/`NO GREAT` rules and asserts the derived gameplay policy disables those judgement windows.
2. Run `gameplay_ruleset_policy_tests` and confirm the missing course-replay setup boundary fails.
3. Add a shared course-replay start-options helper, parse `result.constraintJson` once in the exporter, and install those rules before building each stage policy and materializing input.
4. Re-run the focused policy test to green.

### Task 5: Exclude a recalled saved result from its own previous-best lookup

**Files:**
- Modify: `src/scene/ResultScene.h`
- Modify: `src/scene/ResultScene.cpp`
- Modify: `src/scene/MainMenuScene.h`
- Modify: `src/scene/MainMenuScene.cpp`
- Test: `tests/remote_result_scene_tests.cpp`

1. Add a pure query-selection regression showing a recalled result uses its stored attempt ID as the exclusion, with the selected replay timestamp as the legacy fallback.
2. Run `remote_result_scene_tests` and confirm the query helper is absent/failing.
3. Add `previousBestBeforeCreatedAt` to recalled-result persistence options, centralize previous-best query selection, pass the selected replay summary timestamp through Main Menu recall, and use the result attempt ID even when there is no live completed-attempt object.
4. Re-run the focused result-scene test to green.

### Task 6: Verify and publish the PR update

**Files:** all files above

1. Run all focused targets and their CTest entries.
2. Run `cmake --build cmake-build-debug --target main -j 6`.
3. Run `scripts/ios_firebase_deploy.sh --build-only` for the affected iOS compilation path; do not upload.
4. Review the diff and working tree for unrelated changes.
5. Commit the fixes on `feature/file-based-replays`, push the branch, and report the new commit and checks without resolving review threads.
