# Beatoraja Practice Menu Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the pinned Beatoraja `BMSPlayer.STATE_PRACTICE` menu so gameplay skins receive authentic practice row state, actions, and settings before an attempt begins.

**Architecture:** Keep Beatoraja's mutable `PracticeProperty`, viewport, cursor, and all twelve conditional rows in a focused practice-domain controller. `GamePlayScene` owns the lifecycle boundary: it renders the controller while no attempt/audio is active, forwards committed skin row events to it, and creates the normal attempt only after the source-shaped settings have been applied. The skin bridge only projects captured authority and stages mutations; it never invents menu state or applies a row action synchronously.

**Tech Stack:** C++23, SDL scene lifecycle, existing `practice` domain, selected Lua gameplay skins, CMake/CTest.

**Spec:** [`docs/todo.md`](../../todo.md), pinned Beatoraja `PracticeConfiguration.java`, `BMSPlayer.java`, `PracticeModifier.java`, `GrooveGauge.java`, and `GaugeProperty.java` at `c2ed5db1a46145ed10790c3872f717e95b59db9d`.

## Global Constraints

- Preserve exact source row labels, values, availability, viewport rounding, cursor selection, and event direction (`arg1 >= 0`).
- Do not turn an active Aso practice attempt into `STATE_PRACTICE`; only the actual pre-play controller enables selector `1080` and rows `3000`–`3035`.
- Do not clamp, normalize, or validate a skin event beyond the behavior in the pinned source.
- Keep `vcpkg_installed/` untracked and unstaged.
- Use test-first red/green cycles and one intentional commit per independently usable feature.

---

### Task 1: Source-shaped practice menu domain

**Files:**

- Modify: `src/practice/PracticeConfiguration.h`
- Modify: `src/practice/PracticeConfiguration.cpp`
- Modify: `src/practice/PracticeSession.h`
- Modify: `src/practice/PracticeSession.cpp`
- Modify: `tests/practice_configuration_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**

- Consumes: the active chart's last timeline time, key mode, initial judge rank, total, and play options.
- Produces: `practice::SkinMenuController`, with `setItemScrollPosition(float)`, `changeVisibleItem(std::size_t, bool)`, and `skinMenuState()`; `practice::Session` owns one controller after its source inputs are supplied.

- [x] **Step 1: Write the failing menu-action tests**

  Add a double-play fixture with `lastTimelineMicros = 90'000'000`, `judgeRank = 100`, `total = 200`, `keyMode = 14`, and normal options. Assert the initial ten visible rows, a `1.0F` scroll that selects `GAUGE TYPE`, a row-0 decrement that selects and changes the gauge type, and a row-9 increment that changes `OPTION-DP` from `NORMAL` to `FLIP`. Add a Pop'n fixture asserting `OPTION-1P` wraps among seven source options, and independent fixtures for source time endpoints, total/frequency bounds, graph cycling, and source cursor selection.

- [x] **Step 2: Run the focused test to verify it fails**

  Run: `cmake --build cmake-build-debug --target practice_configuration_tests -j 6 && ./cmake-build-debug/practice_configuration_tests`

  Expected: failure because no retained mutable controller/action interface exists and the current viewport always marks its first visible row as selected.

- [x] **Step 3: Implement the controller from `PracticeConfiguration.java`**

  Add a controller with these source values: start/end time, source gauge type `0..8`, gauge category `FIVEKEYS`/`SEVENKEYS`/`PMS`/`KEYBOARD`/`LR2`, start gauge, judge rank, total, frequency, graph type, 1P random, 2P random, DP flip, cursor ordinal, and item offset. Use ten visible rows; expose `OPTION-2P` and `OPTION-DP` only for 10K/14K/48K. Implement the exact Java actions: time steps and endpoints, nine-way gauge cycle, category reset of its gauge initial value, source gauge maximums, judge/total/frequency steps, three-way graph cycle, Pop'n seven-way 1P option cycle, ten-way player random cycles, and DP toggle. Keep the existing simplified `Configuration` synchronized only for values it can faithfully represent; retain all source-only menu values in the controller for the later application task.

- [x] **Step 4: Run the focused test to verify it passes**

  Run: `cmake --build cmake-build-debug --target practice_configuration_tests -j 6 && ./cmake-build-debug/practice_configuration_tests`

  Expected: `practice configuration tests passed`.

- [x] **Step 5: Commit the reusable domain feature**

  Run: `git add CMakeLists.txt src/practice/PracticeConfiguration.h src/practice/PracticeConfiguration.cpp src/practice/PracticeSession.h src/practice/PracticeSession.cpp tests/practice_configuration_tests.cpp && git commit -m "feat: model Beatoraja practice skin menu"`

### Task 2: Transactional skin events and real menu authority

**Files:**

- Modify: `src/skin/beatoraja/PlaySkinStateBridge.h`
- Modify: `src/skin/beatoraja/PlaySkinStateBridge.cpp`
- Modify: `src/skin/beatoraja/PlaySkinSession.h`
- Modify: `src/skin/beatoraja/PlaySkinSession.cpp`
- Modify: `src/scene/play/GameplaySkinSessionFactory.h`
- Modify: `src/scene/play/GameplaySkinSessionFactory.cpp`
- Modify: `src/scene/play/PlayfieldVisualState.h`
- Modify: `src/scene/play/PlayfieldVisualState.cpp`
- Modify: `tests/play_skin_state_bridge_tests.cpp`
- Modify: `tests/play_skin_session_tests.cpp`

**Interfaces:**

- Consumes: `PlayfieldAuthorityUpdate.practiceMenu` and a new explicit `practiceMenuActive` state captured by the scene.
- Produces: a `SkinFrameMutation` carrying visible practice row index plus increment direction, applied only after the skin frame has submitted; selectors 1080, 3000–3015, and 3020–3035 derive exclusively from `practiceMenuActive`.

- [x] **Step 1: Write failing bridge/session tests**

  Capture an active practice-menu authority with a selected nonzero visible row. Assert `1080`, the selected row's availability, and that row's selection are true while a normal practice attempt keeps them false. Queue event `370 + visibleIndex` with negative and nonnegative `arg1`; assert it stages a practice-row mutation only for the active menu. In the session test, assert the controller callback remains untouched before renderer submission and receives the exact visible index/direction once after successful submission.

- [x] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target play_skin_state_bridge_tests play_skin_session_tests -j 6 && ./cmake-build-debug/play_skin_state_bridge_tests && ./cmake-build-debug/play_skin_session_tests`

  Expected: assertions fail because events 370–385 have no native mutation and every practice selector remains false.

- [x] **Step 3: Implement snapshot-gated mutations**

  Add a source-state boolean beside `practiceMenu`, make its equality explicit, and gate the practice boolean families with it. Add the ordered mutation and its post-submit callback through the bridge, skin session, and factory. The bridge must accept event IDs 370–385 only with an active frame and active menu, use the source direction rule, and otherwise preserve its existing unsupported/event transaction behavior.

- [x] **Step 4: Run focused tests to verify they pass**

  Run: `cmake --build cmake-build-debug --target play_skin_state_bridge_tests play_skin_session_tests -j 6 && ./cmake-build-debug/play_skin_state_bridge_tests && ./cmake-build-debug/play_skin_session_tests`

  Expected: both test executables pass, including rollback/no-submit coverage.

- [x] **Step 5: Commit the skin interaction feature**

  Run: `git add src/skin/beatoraja/PlaySkinStateBridge.h src/skin/beatoraja/PlaySkinStateBridge.cpp src/skin/beatoraja/PlaySkinSession.h src/skin/beatoraja/PlaySkinSession.cpp src/scene/play/GameplaySkinSessionFactory.h src/scene/play/GameplaySkinSessionFactory.cpp src/scene/play/PlayfieldVisualState.h src/scene/play/PlayfieldVisualState.cpp tests/play_skin_state_bridge_tests.cpp tests/play_skin_session_tests.cpp && git commit -m "feat: route practice skin item events"`

### Task 3: Apply the practice controller before starting playback

**Files:**

- Modify: `src/scene/play/GamePlayScene.h`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Modify: `src/scene/ChartViewerScene.cpp`
- Modify: `src/scene/play/GameplayGaugeTypes.h`
- Modify: `src/scene/play/GameplayGaugeRules.cpp`
- Modify: `src/scene/play/GameplayScoreState.h`
- Modify: `tests/practice_configuration_tests.cpp`
- Modify: `tests/playfield_visual_state_tests.cpp`
- Modify: `tests/play_skin_state_bridge_tests.cpp`

**Interfaces:**

- Consumes: the menu controller's complete source property and the selected chart before audio scheduling.
- Produces: a no-audio `STATE_PRACTICE` scene phase that starts a normal practice attempt only after source start input; its attempt chart and `StartOptions` carry the chosen range, playback frequency, random/flip options, total, judge rank, category, and all nine gauge types.

- [x] **Step 1: Write failing lifecycle/application tests**

  Add a scene-authority test that starts a practice scene in menu phase and observes `state_practice` plus source row strings before playback. Add practice-domain tests asserting the source property converts to an attempt plan with a frequency-adjusted range, exactly removes notes outside `[start, end)`, retains the configured TOTAL, and keeps grade gauge types/category identity instead of substituting a normal gauge.

- [x] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target practice_configuration_tests playfield_visual_state_tests play_skin_state_bridge_tests -j 6 && ./cmake-build-debug/practice_configuration_tests && ./cmake-build-debug/playfield_visual_state_tests && ./cmake-build-debug/play_skin_state_bridge_tests`

  Expected: lifecycle tests fail because `GamePlayScene::init()` calls `reset()` and starts audio immediately, and grade/category settings have no Aso attempt representation.

- [x] **Step 3: Implement the source start transition**

  Add a practice-menu phase that constructs presentation/state but does not schedule or play audio. Route source Start input to a transition that reinitializes the chart from the retained pre-modifier source, applies frequency, TOTAL, `PracticeModifier` semantics, 1P/2P random/flip, judge rank, source start time offset, and source end time. Extend Aso gauge representation/rules for the three grade gauges and five source categories using the pinned `GaugeProperty` tables so category and gauge selection affect the actual attempt instead of only the menu text. On returning from a practice attempt, reconstruct the menu from the retained source chart/property, as `BMSPlayer` reloads its model before processing input.

- [x] **Step 4: Run focused tests to verify they pass**

  Run: `cmake --build cmake-build-debug --target practice_configuration_tests playfield_visual_state_tests play_skin_state_bridge_tests -j 6 && ./cmake-build-debug/practice_configuration_tests && ./cmake-build-debug/playfield_visual_state_tests && ./cmake-build-debug/play_skin_state_bridge_tests`

  Expected: all targeted tests pass and show menu authority only before the started attempt.

- [x] **Step 5: Commit the lifecycle/application feature**

  Run: `git add src/scene/play/GamePlayScene.h src/scene/play/GamePlayScene.cpp src/scene/ChartViewerScene.cpp src/scene/play/GameplayGaugeTypes.h src/scene/play/GameplayGaugeRules.cpp src/scene/play/GameplayScoreState.h tests/practice_configuration_tests.cpp tests/playfield_visual_state_tests.cpp tests/play_skin_state_bridge_tests.cpp && git commit -m "feat: start practice from Beatoraja menu state"`

### Task 4: Record compatibility completion and verify both build targets

**Files:**

- Modify: `docs/todo.md`
- Modify: `docs/progress.md`

- [x] **Step 1: Update the source-audited docs**

  Replace the practice-menu checkbox with completed behavior only after all three components are working. State that strings and viewport apply in every BMSPlayer-equivalent state, but event actions and practice booleans are active only during the implemented pre-play menu.

- [x] **Step 2: Run focused and full desktop verification**

  Run: `cmake --build cmake-build-debug --target practice_configuration_tests play_skin_state_bridge_tests play_skin_session_tests main -j 6 && ./cmake-build-debug/practice_configuration_tests && ./cmake-build-debug/play_skin_state_bridge_tests && ./cmake-build-debug/play_skin_session_tests && ctest --test-dir cmake-build-debug --output-on-failure -j 6`

  Expected: all commands succeed.

- [x] **Step 3: Run the non-distribution iOS verification**

  Run: `scripts/ios_firebase_deploy.sh --build-only`

  Expected: `** BUILD SUCCEEDED **`; no Firebase upload occurs.

- [x] **Step 4: Commit the documentation result**

  Run: `git add docs/todo.md docs/progress.md docs/superpowers/plans/2026-08-20-beatoraja-practice-menu.md && git commit -m "docs: record practice skin menu compatibility"`

## Self-review

- Spec coverage: Task 1 covers each `PracticeConfiguration` row and viewport; Task 2 covers `EventFactory`, `BooleanPropertyFactory`, and post-submit mutation timing; Task 3 covers `BMSPlayer` entry/re-entry and `PracticeModifier` application; Task 4 records only verified compatibility.
- Placeholder scan: no `TBD`, unscoped implementation instruction, or omitted test command remains.
- Type consistency: the controller owns source-only property values, the session owns the controller, the scene captures its snapshot, and the skin pipeline forwards a value-owned row mutation back to that session.
