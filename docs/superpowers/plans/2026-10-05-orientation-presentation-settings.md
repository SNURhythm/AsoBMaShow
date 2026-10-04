# Orientation-specific Presentation Settings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve independent portrait/landscape lane and skin settings, provide usable portrait geometry, and place Library and Songs beside each other above a taller Details pane.

**Architecture:** Store two presentation blocks per player and resolve the active block from the actual viewport. Carry orientation explicitly through asynchronous skin ownership and activation; gameplay freezes the active orientation. Use one geometry policy for validation, controls, resets, and rendering.

**Tech Stack:** C++23, SDL, bgfx, Yoga, nlohmann JSON, CMake/CTest; existing Android and iOS platform orientation integration.

**Spec:** `docs/superpowers/specs/2026-10-05-orientation-presentation-settings-design.md`

## Global Constraints

- Portrait geometry: angle 0 degrees, range 0–28; length 16, range 4–32; width 8, range 2–16. Validate projection at the range endpoints before shipping.
- Landscape geometry remains angle 13.4 degrees, range 0–28; length 8, range 5–12; width 8, range 4–12.
- Auto selects the actual viewport; it is not a third settings block. Gameplay retains its orientation through pause/retry.
- Keep calibration, green number/hispeed preferences, input mappings, audio, rulesets, records, installed skin packages, and skin safety policy shared.
- Legacy built-in values migrate to landscape; portrait receives its own defaults. Copy existing skin settings into both orientations once.
- Library/Songs occupy an upper row in portrait, approximately 30%/70% width. Details occupies approximately 40% of usable height below. Landscape retains three columns.
- Use the current checkout. Preserve unrelated iOS project edits; no worktree, deployment, or whole-file formatting. Commit and push verified task changes to the existing upstream.
- Run one build at a time in each build directory and run platform builds sequentially. The user handles iOS device testing.

## Review Focus

- A missing or malformed orientation block must not erase the other block (Task 1).
- A failed delayed skin save after rotation must roll back only its original orientation (Task 2).
- Package replacement/removal must account for skins referenced only by an inactive orientation (Task 2).
- Rotation during text editing, a pending activation, or a profile switch must retain the edit's owner and activate the correct presentation (Task 3).
- Short portrait viewports, safe areas, and long song names must leave lists scrollable and primary actions reachable (Task 5).

---

### Task 1: Presentation ownership, defaults, and storage

**Files:** Create `src/settings/PresentationOrientation.h` and `src/settings/PresentationGeometryPolicy.h`; modify `src/AppSettings.h`, `src/AppSettings.cpp`, `src/AppSettingsStore.h`, `src/AppSettingsStore.cpp`; test `tests/app_settings_store_tests.cpp`, `tests/profile_archive_tests.cpp`, `tests/profile_switch_tests.cpp`. Update existing consumers of moved fields in the same commit to preserve compilation.

**Interfaces:**
- `enum class PresentationOrientation { Landscape = 0, Portrait = 1 };` in namespace `player_settings`.
- `PresentationGeometryPolicy presentationGeometryPolicy(PresentationOrientation)` returns angle/length/width bounds and defaults; each member uses `FloatSettingRange { float minimum, maximum, defaultValue; }`.
- Nested `AppSettings::PresentationSettings` owns the built-in fields listed in the spec and an orientation's `skin::SkinProfileSettings`. Add `presentation()` and `presentation(PresentationOrientation)` accessors, each with mutable and const overloads returning references.
- `AppSettings::activePresentationOrientation() const` and `void setActivePresentationOrientation(PresentationOrientation)` select the runtime block. The selector is not persisted or part of persisted-value equality. Store the blocks directly; do not maintain a second mutable copy of active values.
- Retain `playAreaWidthForKeyMode(int)` and `setPlayAreaWidthForKeyMode(int, float)` as active-presentation convenience methods.

- [x] Add failing migration/round-trip tests: legacy length 11 remains landscape 11, portrait length becomes 16 and angle 0; changing portrait width to 15 retains landscape width 8 after reload; custom skin options initially copy by value and later remain independent. Assert shared calibration and hispeed are unchanged.
- [x] Add failing validation tests: portrait length 32 and width 16 survive save/load; landscape values clamp to 12; nonfinite input receives orientation defaults; a malformed portrait block leaves valid landscape settings intact. Profile copy/export/import must carry both blocks.
- [x] Build and run the affected existing test targets; record the expected failing assertions before implementing.
- [x] Move presentation fields into the nested type, initialize both blocks from the policy, and update consumers to use the accessors. Keep skin safety authoritative at player level; snapshots expose it to existing skin APIs without allowing the two stored blocks to diverge in policy.
- [x] Increment settings schema from 7 to 8. Serialize `presentations.landscape` and `presentations.portrait`; migrate legacy JSON and CFG through the same initialization path. Validate each block against its own policy and retain diagnostics for malformed values.
- [x] Run the migration, settings, profile-switch, and archive tests to passing; commit the independently working storage change.

### Task 2: Orientation-aware skin persistence and transactions

**Files:** Modify `src/ProfileSettingsPersistenceCoordinator.h/.cpp`, `src/skin/SkinCommitCoordinator.h/.cpp`, `src/skin/SkinConfigurationWriteQueue.h/.cpp`, `src/skin/GameplaySkinActivationRequest.h`, `src/skin/package/SkinActivationCommitStore.h`, `src/skin/package/SkinPackageStore.cpp`, and their consumers; test `tests/profile_settings_persistence_tests.cpp`, `tests/skin_commit_coordinator_tests.cpp`, `tests/skin_configuration_write_queue_tests.cpp` `tests/skin_package_store_tests.cpp`, and `tests/skin_package_operation_service_tests.cpp`.

**Interfaces:**
- Append `PresentationOrientation orientation` to `VersionedSkinProfileSettings` and queued configuration writes; production submissions always capture it explicitly.
- Change owner signatures to `snapshot(const SkinProfileId &, PresentationOrientation) const` and `beginCommit(const SkinProfileId &, PresentationOrientation, std::uint64_t expectedGeneration, SkinProfileSettings candidate)`.
- Track skin generations per `(profileId, orientation)`; each full-save job captures generations for both blocks. Inventory snapshots enumerate both orientations while retaining the same player identity.
- Activation requests and prepared commit identity carry orientation independently of content digests. Identical content may keep identical digests.

- [x] Add failing tests that queue a portrait save, switch to landscape, complete the save, and assert only portrait changes. Repeat for save failure, rollback, and an interleaved full settings save; assert neither block is lost.
- [x] Add failing tests for a stale orientation activation and package removal/replacement whose only reference is in the inactive block. A reference in either orientation must participate in the existing package safety transaction.
- [x] Run the three named unit-test targets and the affected existing package lifecycle tests, confirming the new failures.
- [x] Carry orientation through coordinator jobs, optimistic state, rollback, poll results, queue draining, inventory fences, and activation identity. Merge only the intended block; apply results to a live scene only when player and orientation still match. Shared safety-policy changes invalidate both orientation generations through the existing transaction path.
- [x] Update all interface implementations, mocks, controller callers, and configuration-write producers. Ensure replay/export sessions capture their originating presentation and cannot write into the current menu orientation.
- [x] Run the affected persistence, commit, queue, and package tests to passing; commit.

### Task 3: Rotation, profile switches, and editing lifecycle

**Files:** Create `src/settings/PresentationOrientationState.h`; modify `src/main.cpp`, `src/scene/SceneManager.h/.cpp`, `src/scene/ProfileRuntimeReapply.cpp`, `src/scene/ProfileSettingsController.cpp`, `src/scene/SettingsScene.h/.cpp`, `src/scene/SettingsSceneSkins.cpp`, `src/scene/GameplaySkinSettingsController.h/.cpp`, `src/scene/MainMenuScene.cpp`, `src/scene/MusicSelectScene.cpp`, `src/scene/play/PlayfieldPresentationCoordinator.cpp`, and `src/scene/play/ReplayPlayfieldPresentation.cpp`; add `tests/presentation_orientation_state_tests.cpp` and its CMake target; extend `tests/gameplay_skin_settings_tests.cpp`, `tests/profile_runtime_reapply_tests.cpp`, and `tests/scene_language_lifecycle_fixture.cpp`.

**Interfaces:**
- `PresentationOrientationState::updateViewport(int width, int height)` returns whether its resolved orientation changed; invalid or square dimensions retain the previous orientation.
- `void setGameplayLocked(bool)` freezes/resumes viewport selection, and `PresentationOrientation orientation() const` reports the selected block.
- Skin settings controller binding includes `(profileId, orientation, clientId)`; changing either owner component uses the existing cancellation/rebinding lifecycle.

- [x] Add failing tests: landscape → portrait → landscape restores settings; gameplay lock holds portrait through resize/pause/retry; unlocking resolves the newest viewport. Profile switching preserves the runtime viewport selection even though it loads a different settings document.
- [x] Add failing controller tests: a text edit and skin activation initiated in portrait retain their owner after rotation; a late completion cannot reactivate portrait in a landscape scene.
- [x] Run the targeted tests and confirm failures.
- [x] Integrate selection before scene update/input and camera update. On rotation, finish pending edits against their captured block, replace controls, and rebind skins through existing activation/lease lifecycles. Rebuild retained scenes when their owner orientation differs; preserve ordinary menu selection and preview state.
- [x] Display the active orientation beside presentation controls using existing localization conventions. Reset affects the active block only. Handle explicit orientation setting changes using the resulting viewport, not the requested native orientation before it takes effect.
- [x] Run state/controller/profile/lifecycle tests to passing; commit.

### Task 4: Controls, rendering, and touch geometry

**Files:** Create `src/rendering/PortraitPlayfieldFraming.h`; modify `src/scene/SettingsSceneShared.h`, `src/scene/SettingsSceneControls.cpp`, `src/scene/SettingsSceneLayout.cpp`, `src/scene/SettingsSceneSkins.cpp`, `src/main.cpp`, `src/scene/play/BMSRenderer.cpp`, and existing touch-projection consumers; add `tests/portrait_playfield_framing_tests.cpp` with a CMake target; extend `tests/ui_scale_tests.py`, `tests/playfield_projection_tests.cpp`, and `tests/play_skin_touch_geometry_tests.cpp` where applicable.

**Interfaces:**
- `PortraitPlayfieldFrame framePortraitPlayfield(float laneLength, float playAreaWidth, float angleDegrees, float aspect, NormalizedSafeArea safeArea)` returns `cameraDepth` and `lookAtY` for the existing camera angle convention. Define both small input/output structures in the new header and reuse the renderer's actual perspective parameters.
- All active lane inputs and renderer clamps use Task 1's policy. Skin-authored option domains remain unchanged.

- [x] Add failing tests asserting the default 0-degree, length-16, width-8 playfield fits portrait 9:16 and 3:4 viewports, including the judgement line and all playable lanes. Test combinations of each range endpoint, angles 0 and 28, and nonzero safe areas for finite coordinates and visible playable bounds.
- [x] Add tests mapping projected outer-lane centers back to the correct lanes, and assert landscape projection is unchanged. Verify reset and numeric-input acceptance against the same policy.
- [x] Run the geometry tests and confirm the expected clipping/range failures.
- [x] Implement portrait framing by fitting the actual projected playfield bounds within the usable viewport; compute matching rendering and touch transforms. Preserve the existing landscape camera branch. Replace fixed lane bounds/defaults in slider, typed-input, reset, sanitization, and renderer paths with the policy.
- [x] Run geometry and control tests to passing and inspect the default portrait playfield in the desktop app; commit.

### Task 5: Side-by-side selection and taller details

**Files:** Modify `src/scene/MainMenuScene.cpp` and `src/scene/MainMenuScene.h`; extend `tests/ui_scale_tests.py` only if its existing layout fixture can meaningfully verify the real Yoga arrangement.

**Interfaces:** Keep `MainMenuScene::updatePanelLayout()` as the single resize entry point. Name the Library/Songs wrapper `mainMenuBrowser`; maintain one instance of each list and the Details view.

- [x] Add the browser wrapper containing Library and Songs. In portrait, place it above Details; set Library to approximately 30% of browser width and Songs to the remaining width. Keep Library actions vertical so they fit its narrower column.
- [x] Allocate approximately 60% of usable content height to the browser and 40% to Details, accounting for the gap and safe-area padding. Keep detail content scrollable with primary actions pinned. Use flexible/minimum sizes so short portrait windows remain usable; restore existing three-column widths in landscape through the wrapper's row layout.
- [x] Inspect portrait phone and tablet proportions, short portrait windows, and landscape using the real app. Verify long labels, empty library, selected chart, scrolled lists, modal opening, and rotation without selection/preview resets. Check Start and Settings remain reachable.
- [x] Run `python3 tests/ui_scale_tests.py` and the existing main-menu lifecycle checks; commit the verified layout change. Do not add source-text-only tests that merely repeat constants.

### Task 6: Integrated verification and handoff

**Files:** Only fixes within the preceding task scope, plus this plan's completion checkboxes.

- [ ] Run `git diff --check`; build `main` and all affected test targets in one `cmake --build cmake-build-debug --target ... -j 6` invocation. Run focused tests before `ctest --test-dir cmake-build-debug --output-on-failure -j 6`.
- [ ] Investigate failures using their actual output. The prior run had a reproducible, unchanged Metal SDF-shadow failure; do not label a new failure pre-existing without checking the affected source and baseline evidence.
- [ ] Run `scripts/android_firebase_deploy.sh --build-only` after desktop compilation finishes. Leave iOS device validation to the user and report whether any iOS compile check was performed. Do not run a distribution action.
- [ ] Obtain the execution method's final review, address findings, and rerun only affected checks. Verify separate settings after restart and the final portrait menu visually.
- [ ] Commit remaining verified fixes, push the current branch upstream, and report results plus any unresolved platform/test limitations. Leave unrelated iOS project changes unstaged.
