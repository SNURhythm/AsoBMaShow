# Synthetic Skinned Replay Ghosts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render option-controlled synthetic replay ghost outlines with each selected skin's per-lane note geometry in replay watch and replay-video export.

**Architecture:** Keep `ReplayGhostUtils` as the single replay-event authority, add a model-backed replay-event overload for both interactive replay and export, and route the immutable event list through `PlayfieldPresentationCoordinator`.  The Lua session exposes only per-lane ghost geometry and submits the overlay through its existing UI quad renderer after a successful skin frame; built-in playback continues to own its established ghost renderer.

**Tech Stack:** C++20, bgfx, existing `SkinQuadBatchRenderer`, Lua gameplay-skin renderer, CTest, unsigned iOS release verification.

## Global Constraints

- Pinned Beatoraja source at `c2ed5db1a46145ed10790c3872f717e95b59db9d` remains the source of truth for skin semantics; this overlay is explicitly application-synthetic.
- The existing `PlayfieldPresentationConfig::replayGhostRenderingEnabled` and `ReplayVideoExportOptions::renderReplayGhosts` are the only controls.
- Every ghost must use its own lane's x-position, width, normal-note height, clip, scroll origin, and active UI transform; never use lane zero geometry as another lane's template.
- Submit only after a selected skin has submitted successfully.  Do not create built-in/skin hybrids or alter failure diagnostics.
- Do not add synthetic touch points or miss markers.
- Desktop is the primary loop.  Use incremental `cmake --build cmake-build-debug --target main -j 12`; finish native-facing changes with `scripts/ios_release_verify.sh` and never deploy/upload.

---

### Task 1: Share replay ghost timeline data without parser ownership

**Files:**
- Modify: `src/ReplayGhostUtils.h`
- Create: `src/ReplayGhostUtils.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Modify: `src/scene/play/PlayfieldProjection.h`
- Modify: `src/scene/play/ReplayPlayfieldPresentation.{h,cpp}`
- Modify: `src/scene/play/GamePlayScene.cpp`
- Test: `tests/replay_playfield_presentation_tests.cpp`

**Interfaces:**
- Consumes: `ReplayData`, `PlayfieldChartVisualModel::timelines`, `PlayfieldChartVisualModel::laneOrder`, and `scrollPositionAtTime`.
- Produces: `std::vector<ReplayGhostEvent> replay_ghost::buildReplayGhostEvents(const ReplayData &, const PlayfieldChartVisualModel &)` for replay watch and export construction.

- [ ] **Step 1: Write failing model-timeline ghost tests**

  Add a chart-visual model with raw lanes `{4, 1}` and two timeline times to
  `replay_playfield_presentation_tests`.  Assert that the model overload admits
  only a judged replay event on a known timeline/lane, preserves raw lane `4`,
  records `judgeScrollPosition`, and rejects unknown lanes/times:

  ```cpp
  const auto ghosts = replay_ghost::buildReplayGhostEvents(replay, model);
  expect(ghosts.size() == 1 && ghosts.front().lane == 4 &&
             ghosts.front().judgeScrollPosition == 2.5,
         "model-backed replay ghosts retain the raw skin lane");
  ```

  In `replay_playfield_presentation_tests`, create a selected-skin replay fixture with one valid replay ghost and assert the coordinator dependency receives one event before its first frame.

- [ ] **Step 2: Run the focused tests and verify RED**

  Run:

  ```bash
  cmake --build cmake-build-debug --target replay_playfield_presentation_tests -j 12 > /tmp/skinned-ghost-task1-red.log 2>&1 && ./cmake-build-debug/replay_playfield_presentation_tests
  ```

  Expected: compile failure because the model overload and coordinator input do not exist.

- [ ] **Step 3: Add the shared model-backed event overload**

  Refactor the existing parser-backed helper so both overloads use one internal
  event-building routine taking `std::span<const ReplayGhostTimeline>`, raw
  playable lanes, and a scroll-position callback.  Define the value type and
  model overload in `ReplayGhostUtils.cpp`; the header keeps the public value
  API without importing parser ownership:

  ```cpp
  [[nodiscard]] std::vector<ReplayGhostEvent>
  buildReplayGhostEvents(const ReplayData &replay,
                         const PlayfieldChartVisualModel &model);

   // ReplayGhostUtils.cpp
   std::vector<ReplayGhostEvent> buildReplayGhostEvents(
       const ReplayData &replay, const PlayfieldChartVisualModel &model) {
    return buildReplayGhostEvents(replay, makeReplayGhostTimelines(model),
                                  model.laneOrder, [&model](long long time) {
      return scrollPositionAtTime(model, time);
    });
  }
  ```

  Use this overload only when replay data is present: pass its immutable result
  into `PlayfieldPresentationCoordinatorDependencies` from both
  `GamePlayScene` and `ReplayPlayfieldPresentation::create`.  Keep
  `BMSRenderer::setReplayData` unchanged so built-in rendering remains its
  established path.

- [ ] **Step 4: Run focused tests and verify GREEN**

  Run the command from Step 2.  Expected: both executables pass and the old
  parser-backed ghost tests still pass.

- [ ] **Step 5: Commit the timeline slice**

  ```bash
  git add src/ReplayGhostUtils.h src/ReplayGhostUtils.cpp src/CMakeLists.txt CMakeLists.txt \
    src/scene/play/PlayfieldProjection.h \
    src/scene/play/ReplayPlayfieldPresentation.h \
    src/scene/play/ReplayPlayfieldPresentation.cpp src/scene/play/GamePlayScene.cpp \
    tests/replay_playfield_presentation_tests.cpp
  git commit -m "refactor: share replay ghost timeline data"
  ```

### Task 2: Publish and submit per-lane selected-skin ghost geometry

**Files:**
- Modify: `src/scene/play/CoordinatedPlaySkinSession.h`
- Modify: `src/skin/beatoraja/Skin2DRenderer.{h,cpp}`
- Modify: `src/skin/beatoraja/PlaySkinSession.{h,cpp}`
- Modify: `src/skin/beatoraja/Skin2DRendererSubmit.cpp`
- Create: `src/scene/play/SyntheticReplayGhostOverlay.{h,cpp}`
- Modify: `src/scene/play/CMakeLists.txt`
- Modify: `CMakeLists.txt`
- Test: `tests/play_skin_session_tests.cpp`

**Interfaces:**
- Consumes: the selected `SkinNoteObject`, active destination offsets, skin viewport, current projection scroll position/hispeed, and immutable `ReplayGhostEvent` values.
- Produces: `skin::SyntheticReplayGhostGeometry` with one `lane` entry per selected skin lane, and `CoordinatedPlaySkinSession::submitSyntheticReplayGhosts(RenderContext &, SyntheticReplayGhostFrameInput)`.

- [ ] **Step 1: Write failing per-lane geometry tests**

  Build a selected skin whose lane 0 is `{x=10, y=100, width=30,
  noteHeight=8}` and lane 1 is `{x=90, y=250, width=52, noteHeight=16}`.
  Assert that the published geometry contains both lanes with those distinct
  values after one prepared frame.  Assert the transformed outline for a lane
  1 ghost uses x=90/width=52/noteHeight=16 and is clipped to lane 1's play
  region rather than lane 0's.

  ```cpp
  expect(geometry.lanes.at(1).noteRect.width == 52.0 &&
             geometry.lanes.at(1).noteRect.height == 16.0,
         "synthetic ghost geometry remains lane-specific");
  ```

- [ ] **Step 2: Run focused tests and verify RED**

  Run:

  ```bash
  cmake --build cmake-build-debug --target play_skin_session_tests replay_playfield_presentation_tests -j 12 > /tmp/skinned-ghost-task2-red.log 2>&1 && ./cmake-build-debug/play_skin_session_tests && ./cmake-build-debug/replay_playfield_presentation_tests
  ```

  Expected: compile failure because no selected-session geometry or overlay
  submission interface exists.

- [ ] **Step 3: Add geometry publication and the UI overlay**

  Add value-only geometry types to `CoordinatedPlaySkinSession.h`:

  ```cpp
  struct SyntheticReplayGhostLaneGeometry {
    int lane = -1;
    skin::AuthoredRect laneClip;
    skin::AuthoredRect normalNote;
    skin::Affine2D authoredToUi;
    double scrollPixelsPerUnit = 0.0;
  };

  struct SyntheticReplayGhostFrameInput {
    std::uint64_t frameSerial = 0;
    double currentScrollPosition = 0.0;
    bool enabled = false;
    std::span<const ReplayGhostEvent> events;
  };
  ```

  During `Skin2DRenderer::evaluateFrame`, calculate the geometry from the
  selected Note object's exact resolved lane presentations.  Include each
  lane's normal-note height, lane destination/clip, active offsets, and
  `authoredToUi`; derive scroll pixels per unit with the same shared scroll
  conversion already used by `lowerNoteObject`.  Publish it only with a
  successful evaluation and clear it whenever the frame is discarded.

  `PlaySkinSession` retains that geometry only for the matching successful
  frame.  Implement `submitSyntheticReplayGhosts` using a small
  `SyntheticReplayGhostOverlay` that emits four `SkinPrimitiveCommand` bars
  per outline through `SkinQuadBatchRenderer` on `rendering::ui_view`, after
  the session's normal skin submission.  Reuse the existing white/early-blue/
  late-red colors and `ReplayGhostEvent` time test.  Clamp each outline to its
  own lane clip; omit invalid/missing geometry without changing the frame
  result.

- [ ] **Step 4: Run focused tests and verify GREEN**

  Run the command from Step 2.  Expected: one lane-1 outline is submitted with
  lane-1 geometry, the unchanged normal skin command path still passes, and a
  discarded skin frame publishes no geometry.

- [ ] **Step 5: Commit the geometry/overlay slice**

  ```bash
  git add src/scene/play/CoordinatedPlaySkinSession.h \
    src/scene/play/SyntheticReplayGhostOverlay.h \
    src/scene/play/SyntheticReplayGhostOverlay.cpp src/scene/play/CMakeLists.txt \
    src/skin/beatoraja/Skin2DRenderer.h src/skin/beatoraja/Skin2DRenderer.cpp \
    src/skin/beatoraja/Skin2DRendererSubmit.cpp \
    src/skin/beatoraja/PlaySkinSession.h src/skin/beatoraja/PlaySkinSession.cpp \
    tests/play_skin_session_tests.cpp CMakeLists.txt
  git commit -m "feat: render synthetic ghosts with skin geometry"
  ```

### Task 3: Activate the overlay from the shared coordinator and verify all consumers

**Files:**
- Modify: `src/scene/play/PlayfieldPresentationCoordinator.{h,cpp}`
- Modify: `tests/playfield_presentation_coordinator_tests.cpp`
- Modify: `tests/replay_playfield_presentation_tests.cpp`
- Modify: `docs/superpowers/specs/2026-08-10-synthetic-skinned-replay-ghosts-design.md`

**Interfaces:**
- Consumes: `PlayfieldPresentationCoordinatorDependencies::replayGhostEvents`, `PlayfieldPresentationConfig::replayGhostRenderingEnabled`, and the selected session's `submitSyntheticReplayGhosts`.
- Produces: one option-controlled selected-skin overlay path used by replay watch and by normal/course `ReplayPlayfieldPresentation` frames.

- [ ] **Step 1: Write failing coordinator integration tests**

  Extend the selected-session fake with an overlay recorder.  Prepare and
  render three frames:

  ```cpp
  configuration.replayGhostRenderingEnabled = false;
  coordinator.configure(configuration);
  coordinator.prepareFrame(state, projection);
  coordinator.render(context);
  expect(fake.overlayCalls == 0, "option-off suppresses selected-skin ghosts");

  configuration.replayGhostRenderingEnabled = true;
  coordinator.configure(configuration);
  coordinator.prepareFrame(state, projection);
  coordinator.render(context);
  expect(fake.overlayCalls == 1 && fake.lastOverlay.events.size() == 1,
         "option-on submits one selected-skin replay overlay");
  ```

  Add a no-skin case proving the coordinator never invokes the synthetic path;
  the built-in renderer remains responsible for its pre-existing ghosts.  Add
  a replay-presentation case proving normal and course export use the same
  coordinator dependency; replay watch uses that same concrete coordinator
  construction in `GamePlayScene`.

- [ ] **Step 2: Run focused tests and verify RED**

  Run:

  ```bash
  cmake --build cmake-build-debug --target playfield_presentation_coordinator_tests replay_playfield_presentation_tests -j 12 > /tmp/skinned-ghost-task3-red.log 2>&1 && ./cmake-build-debug/playfield_presentation_coordinator_tests && ./cmake-build-debug/replay_playfield_presentation_tests
  ```

  Expected: assertions fail because the coordinator does not retain replay
  ghost events or dispatch a successful selected-skin overlay.

- [ ] **Step 3: Dispatch only after successful selected-skin submission**

  Store the immutable events and the configured option in the coordinator.
  Extend `PendingFrame` with the matching frame serial and current scroll
  position.  Immediately after `skin_->render(...)` returns a successful
  `PresentationMode::Skin` result, call:

  ```cpp
  skin_->submitSyntheticReplayGhosts(
      context, {.frameSerial = pending.frameSerial,
                .currentScrollPosition = pending.currentScrollPosition,
                .enabled = replayGhostRenderingEnabled_,
                .events = replayGhostEvents_});
  ```

  Do not call it for a recoverable/critical/no-submission result, built-in
  path, or option-off.  Do not mutate a frame result or diagnostic if optional
  overlay submission has no usable geometry.  Update the design doc with the
  final method/type names if they differ from this plan.

- [ ] **Step 4: Run the focused tests and the incremental application build**

  Run the command from Step 2, then:

  ```bash
  cmake --build cmake-build-debug --target main -j 12 > /tmp/skinned-ghost-main.log 2>&1
  ```

  Expected: all focused tests and the desktop application link successfully.

- [ ] **Step 5: Run full verification and commit**

  Run:

  ```bash
  ctest --test-dir cmake-build-debug --output-on-failure -j 12 > /tmp/skinned-ghost-ctest.log 2>&1
  scripts/ios_release_verify.sh > /tmp/skinned-ghost-ios.log 2>&1
  git diff --check
  ```

  Expected: all desktop tests, unsigned iOS verification, and whitespace
  checks pass.  Then commit:

  ```bash
  git add src/scene/play/PlayfieldPresentationCoordinator.h \
    src/scene/play/PlayfieldPresentationCoordinator.cpp \
    tests/playfield_presentation_coordinator_tests.cpp \
    tests/replay_playfield_presentation_tests.cpp \
    docs/superpowers/specs/2026-08-10-synthetic-skinned-replay-ghosts-design.md
  git commit -m "feat: enable synthetic replay ghosts for skins"
  ```
