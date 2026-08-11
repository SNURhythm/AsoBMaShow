# Beatoraja Hi-Speed Compatibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make gameplay, replay watch, replay export, and skin properties use Beatoraja’s fixed/off Hi-Speed, live green-number, and cover auto-adjust semantics.

**Architecture:** A source-faithful runtime Hi-Speed state owns fixed/off mode, target BPM, raw Hi-Speed, manual change step, and lane-cover resets. Presentations receive its current Hi-Speed directly; built-in labels and Lua properties use one `LaneRenderer.currentduration` helper.

**Tech Stack:** C++20, existing CMake tests, pinned Beatoraja `c2ed5db1a46145ed10790c3872f717e95b59db9d`.

## Global Constraints

- Treat pinned Beatoraja `PlayConfig`, `LaneRenderer`, `ControlInputProcessor`, and `IntegerPropertyFactory` as the behavior source.
- Preserve unrelated `android/build/`; use `cmake-build-debug` with filtered `-j 12` output.
- Use test-first changes; verify desktop then unsigned iOS; do not deploy.

---

### Task 1: Source-faithful state and settings

**Files:**

- Create: `src/scene/play/BeatorajaHiSpeed.h`, `src/scene/play/BeatorajaHiSpeedChart.{h,cpp}`
- Modify: `src/AppSettings.h`, `src/AppSettings.cpp`, `src/AppSettingsStore.h`, `src/AppSettingsStore.cpp`
- Test: `tests/beatoraja_hispeed_tests.cpp`, `tests/app_settings_store_tests.cpp`

**Interfaces:** `gameplay_hispeed::State` provides `changeHispeed`,
`setDurationMilliseconds`, `setLaneCover`, and `setLaneCoverEnabled`, with
`FixMode::{Off,Start,Max,Main,Min}`.

- [x] Write red tests for the fixed Main formula, cover reset, current-BPM auto reset, toggle-without-reset, and Off-mode raw Hi-Speed.
- [x] Build `beatoraja_hispeed_tests`; verify those assertions fail because the state is absent.
- [x] Implement only the state transitions from `LaneRenderer` and a schema migration that replaces the old multiplier approximation with source-compatible settings.
- [x] Run `beatoraja_hispeed_tests` and `app_settings_store_tests` green.
- [x] Commit with the completed compatibility slice.

### Task 2: Propagate current Hi-Speed through live, replay, and export paths

**Files:**

- Modify: `src/scene/play/GamePlayScene.cpp`, `src/scene/play/GamePlayScene.h`, `src/scene/play/PlayfieldVisualState.h`
- Modify: `src/scene/play/ReplayPlayfieldPresentation.cpp`, `src/scene/play/ReplayVideoGameplayPreflight.cpp`, `src/ReplayVideoExporter.cpp`
- Test: `tests/replay_playfield_presentation_tests.cpp`, `tests/playfield_visual_state_tests.cpp`

**Interfaces:** `PlayfieldPresentationConfig` carries the exact `configuredHispeed` from Task 1, not a reference-BPM multiplier.

- [x] Write red live/replay tests proving auto-adjust uses current BPM, normal cover uses selected fixed BPM, and a cover toggle does not reset Hi-Speed.
- [x] Build `replay_playfield_presentation_tests`; verify the old multiplier reconstruction fails the new expectations.
- [x] Propagate one state object through normal gameplay, replay watch, normal export, and course export.
- [x] Run focused replay and visual-state tests green.
- [x] Commit with the completed compatibility slice.

### Task 3: Use the live current duration for all green-number output

**Files:**

- Modify: `src/scene/play/BMSRenderer.cpp`, `src/scene/play/BMSRenderer.h`, `src/scene/play/PlayfieldProjection.h`
- Modify: `src/skin/beatoraja/PlaySkinStateBridge.cpp`
- Modify: `src/scene/SettingsSceneControls.cpp`, `src/scene/SettingsSceneLayout.cpp`, `src/scene/SettingsSceneShared.h`
- Test: `tests/builtin_renderer_characterization_tests.cpp`, `tests/play_skin_state_bridge_tests.cpp`

**Interfaces:** `gameplay_visible_time::currentDurationMilliseconds` is the sole C++ equivalent of `LaneRenderer.currentduration`; selector 313 and the built-in lane-cover label both call it.

- [x] Build the existing 500-ms / Hi-Speed-6.06 regression; verify its initial failure.
- [x] Use the helper in both renderer and skin bridge, and expose all source fixed modes in Settings.
- [x] Run renderer and skin-state bridge tests green; the constant-BPM case reports green 198 everywhere.
- [x] Commit with the completed compatibility slice.

### Task 4: Verification

- [x] Run `cmake --build cmake-build-debug -j 12` and `ctest --test-dir cmake-build-debug --output-on-failure -j 12`.
- [x] Run `scripts/ios_release_verify.sh`.
- [x] Record results in this plan and commit the compatibility slice.

## Verification record (2026-08-11)

- Focused desktop tests passed: `beatoraja_hispeed_tests`,
  `app_settings_store_tests`, `replay_playfield_presentation_tests`,
  `beatoraja_replay_codec_tests`, and `builtin_renderer_characterization_tests`.
- `cmake --build cmake-build-debug --target main … -j 12` passed, followed by
  `ctest --test-dir cmake-build-debug --output-on-failure -j 12`: **260/260**.
- The real Lua-skins-disabled `main` target linked with
  `cmake --build cmake-build-lua-off-vcpkg --target main -j 12`.
- `scripts/ios_release_verify.sh` passed: native release-critical tests
  **61/61**, unsigned arm64 `BUILD SUCCEEDED`, and artifact audit passed.
