# Note Customization Review Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Address the verified findings in PR #123 without changing the user's chosen popup workflow.

**Architecture:** Share immutable note-style maps in presentation snapshots and reuse unchanged preview style snapshots. Keep the editor's presentation mode separate from the mode that owns saved styles. Extend the existing preview flow to all supported appearance modes, allocating enough timeline lanes.

**Tech Stack:** C++23, SDL/bgfx, CMake/CTest, Python behavioral fixtures.

**Spec:** PR #123 review threads r4186408405, r4186730705, r4186898860; the user's request for a popup with note samples and Confirm. The user explicitly selected live gameplay feedback while dragging, with restoration on Cancel, for popup review r4189883903.

## Global Constraints

- Work in the current checkout; preserve the unrelated iOS project edit.
- Do not edit amalgamated parser files or apply whole-file formatting.
- Run one build at a time in cmake-build-debug; use six jobs for build/tests.
- Commit and push verified task changes to the current upstream. No deployment.

## Review Focus

- Already captured frames must retain old styles after a settings change (Task 1).
- Repeated preview synchronization must not recreate unchanged maps (Task 1).
- Follow must affect storage without changing visible lanes or sharing selection (Task 2).
- Scratchless independent settings must still be editable when Follow is off (Task 2).
- 24K/48K timelines and gameplay input must include the highest lane safely (Task 3).

### Task 1: Remove per-frame note-style allocations

**Files:** `src/settings/BuiltInNotes.h`, `src/scene/play/PlayfieldVisualState.h`, `src/scene/play/BMSRenderer.{h,cpp}`, gameplay/preview/replay configuration builders, `src/scene/SettingsScene.h`, `tests/playfield_visual_state_tests.cpp`, renderer test setup, `CMakeLists.txt`.

**Interfaces:** `SharedModeStyles = std::shared_ptr<const ModeStyles>`; `snapshotModeStyles(const ModeStyles &, const SharedModeStyles &previous = {})`; a `resolve` overload for shared snapshots. SettingsScene retains the previous shared style snapshot.

- [x] Add an allocation-failure regression around copying a fully populated 48-lane presentation configuration; run and observe failure with the current embedded maps.
- [x] Share immutable styles through configurations and renderer; reuse equal preview maps and preserve defaults for empty snapshots.
- [x] Verify snapshot reuse, old-frame immutability, reset-to-default resolution, and renderer appearance.
- [x] Build/run `playfield_visual_state_tests` and `builtin_renderer_characterization_tests`; expect pass.

### Task 2: Preserve scratchless editor identity

**Files:** `src/scene/SettingsSceneControls.cpp`, `tests/settings_note_editing_tests.py`, `CMakeLists.txt`.

**Interfaces:** `appendBuiltInNoteControls` retains `keyMode` for lanes/selection and uses a separate `settingsKeyMode` for edits.

- [x] Execute the production selection/apply preparation in a behavioral fixture, with Follow on/off for -5/-7 and parent selections already populated; expect only 5/7 key lanes and independent selection identities. Observe failure before the fix.
- [x] Separate the storage mode from the presentation mode.
- [x] Verify edits persist to the parent only with Follow enabled, preserve other lanes, and trigger preview synchronization. Run `settings_note_editing_tests`; expect pass.

### Task 3: Expose every supported appearance mode

**Files:** `src/scene/SettingsPreviewChart.{h,cpp}`, `tests/settings_preview_chart_tests.cpp`, renderer characterization if needed.

**Interfaces:** Existing `kPreviewKeyModes` and `makePreviewChart(int)` cover 9, 24, and 48; timelines size from the actual maximum lane, retaining legacy 16-lane capacity.

- [x] Add explicit 9K/24K/48K construction coverage with literal lane counts and highest-lane assertions; observe failure before the change.
- [x] Expand the selector modes and timeline allocation, and update tests that incorrectly assume every chart has 16 lanes.
- [x] Run preview-chart tests and renderer characterization across all modes; expect pass.

### Task 4: Preview color drafts without saving them

**Files:** `src/view/ColorPickerPopup.{h,cpp}`, `src/scene/SettingsScene{,Controls,Preview}.{h,cpp}` as applicable, popup/renderer/rotation tests and extraction helpers.

**Interfaces:** The popup accepts an optional RGB draft callback. SettingsScene retains a configuration override callback, applies it after building the saved preview configuration, and clears it when closing the popup. Each editor prepares its override when the color changes, so rendering reuses the immutable note snapshot.

- [x] Reproduce missing paused-preview propagation and restoration using production scene methods in the renderer fixture.
- [x] Forward popup changes, apply note/judge/measure drafts only to preview configuration, and clear overrides on all close paths.
- [x] Verify drag callbacks, paused preview, original settings preservation, Confirm, Cancel/Escape, and rotation; run popup, renderer, and rotation tests.

### Finish

- [x] User selected live gameplay preview while dragging with restoration on Cancel; implement Task 4.
- [x] Build `main` and changed test targets, run full CTest with `-j 6`, review the final diff, and fix substantive findings.
- [x] Commit and push task changes. Report any review feedback intentionally left unchanged. Posting review replies requires explicit authorization.


## Verification record

- The allocation regression failed before sharing styles, then passed with immutable snapshots; unchanged-cache, captured-frame, and reset checks passed.
- Scratchless Follow regression failed on the extra lane, then passed for -5/-7 with Follow enabled and disabled.
- Explicit 9K/24K/48K construction failed before mode expansion; chart, touch, autoplay, and renderer checks now pass across every preview mode.
- Paused color preview and restoration assertions failed before the override was wired, then passed. Popup callbacks and rotation cleanup passed.
- Desktop main build and all six focused CTest groups passed.
- Independent review found no correctness defect. The remaining editor-to-popup integration coverage was completed during the subsequent review loop below.
- Full CTest: 425/425 passed in 111.55 seconds; `git diff --check` passed.

## Review loop

- A fresh independent review of the complete PR found no actionable production defects.
- Replaced the manually injected draft test with actual note, judge-line, and measure-line editor construction, palette clicks, popup events, and scene result dispatch against the real renderer and settings.
- The integration test covers draft propagation before release, paused preview, Confirm/Cancel, persistence counts, callback/portal cleanup, preserved dimensions and unselected lanes, scratchless Follow storage, and reopening rebuilt controls with the correct color.
- A fresh independent review of the final test changes found no actionable issues.
- Desktop build and the expanded integration test passed; the complete CTest suite passed again (425/425). `git diff --check` passed.
