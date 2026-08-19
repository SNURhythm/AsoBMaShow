# PR 99 Review Round Two Implementation Plan (Complete)

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix every valid newly-opened PR 99 review thread without changing already-correct compatibility behavior.

**Architecture:** Keep profile switching as the lifetime boundary for asynchronous IR work, save skin-originated volume changes at the same bounded scene lifecycle point as other gameplay settings, and preserve replay/live display-timing parity through the shared presentation configuration and projection request. Keep the duration migration retryable when its chart metadata dependency cannot be opened, and route FFmpeg-decoded WebP images through the existing common resize boundary.

**Tech Stack:** C++23, SQLite, CTest, Xcode build-only verification.

**Spec:** The five unresolved GitHub review threads fetched for PR 99 on 2026-08-20.

## Global Constraints

- Preserve the pinned Beatoraja behavior and use no substitute skin values.
- Commit each independently testable review fix separately; keep `vcpkg_installed/` untracked.
- Write and run a focused failing regression before each production fix.
- Resolve a GitHub review thread only after its focused verification passes.

---

### Task 1: Quiesce account lookup before profile activation

**Files:**
- Modify: `src/context.h`
- Test: `tests/ir_account_lookup_service_tests.cpp`

- [x] Verify the existing active-lookup cancellation coverage proves service reset joins a running lookup before it can publish stale state.
- [x] Run `ir_account_lookup_service_tests` and inspect the profile-pause wiring, which previously omitted the account lookup service.
- [x] Reset the account lookup service in `pauseIrProfileServices()` after cancelling submission and ranking work, so joining it occurs before `ProfileSessionCoordinator` changes `activeProfile`.
- [x] Re-run `ir_account_lookup_service_tests`, compile the context consumers, commit `fix: quiesce IR account lookup before profile switches`, and resolve the thread.

### Task 2: Persist gameplay-skin audio settings

**Files:**
- Modify: `src/scene/play/GamePlayScene.{h,cpp}`
- Test: existing gameplay settings/lifecycle coverage where feasible

- [x] Identify the existing play-skin session coverage and scene lifecycle path that exercise skin-originated audio changes.
- [x] Inspect the persistence path and confirm changed skin volume values previously only affected the live audio engine.
- [x] Track a dirty volume write in `applySkinAudioVolume()` and flush it at result transition and scene destruction without introducing a per-frame disk write.
- [x] Run the focused gameplay tests and scene compile, commit `fix: persist gameplay skin volume settings`, and resolve the thread.

### Task 3: Defer failed score-duration migration attachments

**Files:**
- Modify: `src/repositories/ScoreRepositorySchema.cpp`
- Test: `tests/score_provenance_db_tests.cpp`

- [x] Add a v13 migration fixture whose chart database cannot be attached and assert schema version 13 is not recorded.
- [x] Run `score_provenance_db_tests` and observe the current migration advances its version.
- [x] Return the existing retry/defer signal when chart attachment fails, retaining version 12 so a later `EnsureSchema` can retry.
- [x] Re-run `score_provenance_db_tests`, commit `fix: retry score duration migration after attach failures`, and resolve the thread.

### Task 4: Share configured display timing with all gameplay surfaces

**Files:**
- Modify: `src/scene/play/PlayfieldVisualState.h`, `src/scene/play/GamePlayScene.cpp`, `src/scene/play/ReplayVideoGameplayPreflight.cpp`, `src/ReplayVideoExporter.cpp`, `src/skin/beatoraja/PlaySkinStateBridge.cpp`
- Test: `tests/play_skin_state_bridge_tests.cpp`, `tests/replay_playfield_presentation_tests.cpp`

- [x] Add regressions for integer property 12 and replay projection requests using a nonzero configured display timing.
- [x] Run the focused tests and observe the property lacks the configured timing value.
- [x] Add the milliseconds setting to `PlayfieldPresentationConfig`, refresh it after live auto-adjustment, project it through the replay config, and pass the derived note-display time to single-chart and course export frames.
- [x] Re-run the focused bridge/replay tests, commit `fix: propagate display timing across gameplay surfaces`, and resolve the thread.

### Task 5: Resize FFmpeg WebP fallback output

**Files:**
- Modify: `src/view/ImageFileDecoder.cpp`
- Test: `tests/image_file_decoder_tests.cpp`

- [x] Extend the FFmpeg-only WebP fallback fixture with a one-pixel target and assert the decoded image is resized.
- [x] Run `image_file_decoder_tests` and observe the full-size fallback result.
- [x] Route successful fallback output through `resize()` before returning it.
- [x] Re-run `image_file_decoder_tests`, commit `fix: resize FFmpeg WebP fallback output`, and resolve the thread.

## Final Verification

- [x] Run `cmake --build cmake-build-debug --target main -j 6`.
- [x] Run `ctest --test-dir cmake-build-debug --output-on-failure -j 6` (266/266 passed).
- [x] Run `scripts/ios_firebase_deploy.sh --build-only` (BUILD SUCCEEDED).
- [ ] Confirm there are no unresolved PR 99 review threads, commit this completed plan separately, and push `feature/skin-compat`.
