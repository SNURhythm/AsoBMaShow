# Beatoraja Gameplay-Skin Visualizers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement every specialized graph and timing visualizer accepted by the pinned Beatoraja gameplay loaders.

**Architecture:** Publish source-shaped, immutable graph authority from gameplay and project it through `PlaySkinStateBridge`. Each visualizer receives a typed canonical model payload and emits existing bounded primitive or textured draw commands; it never reconstructs chart history during rendering.

**Tech Stack:** C++23, existing gameplay simulation/visual snapshots, bgfx skin command pipeline, CMake/CTest.

**Spec:** [`docs/superpowers/specs/2026-08-21-beatoraja-gameplay-skin-parity-design.md`](../specs/2026-08-21-beatoraja-gameplay-skin-parity-design.md)

## Global Constraints

- Match the pinned Java defaults, buffer sizes, ordering, decay, reveal, cursor, color, and reset behavior.
- Use the exact 100-entry recent-judgement behavior where the pinned visualizer does.
- Keep graph histories bounded and incrementally updated outside the draw loop.
- Extend Lua decoding in each feature task; JSON and LR2 frontends consume the finished typed payloads later.
- Do not replace valid graph objects with `SkinBlankObject`.
- Keep `vcpkg_installed/` untracked and do not run a whole-file formatter.
- Use a red/green test cycle and one graph-family commit per task.

---

### Task 1: Immutable gameplay graph authority

**Files:**

- Create: `src/scene/play/SkinGameplayGraphState.h`
- Create: `src/scene/play/SkinGameplayGraphState.cpp`
- Modify: `src/scene/play/PlayfieldVisualState.h`
- Modify: `src/scene/play/PlayfieldVisualState.cpp`
- Modify: `src/scene/play/PlayfieldChartVisualModel.h`
- Modify: `src/scene/play/PlayfieldChartVisualModel.cpp`
- Modify: `src/scene/play/GameplaySimulation.h`
- Modify: `src/scene/play/GameplaySimulation.cpp`
- Modify: `src/skin/beatoraja/Skin2DRenderer.h`
- Modify: `src/skin/beatoraja/PlaySkinStateBridge.h`
- Modify: `src/skin/beatoraja/PlaySkinStateBridge.cpp`
- Modify: `tests/playfield_visual_state_tests.cpp`
- Modify: `tests/gameplay_simulation_tests.cpp`
- Modify: `tests/play_skin_state_bridge_tests.cpp`

**Interfaces:**

- Consumes: chart timelines, note identities, accepted judgements/timing differences, scroll/BPM/stop changes, and gauge samples.
- Produces: `SkinGameplayGraphState` in every immutable `PlayfieldVisualState` and a span-only `SkinGameplayGraphStateView` from `ISkinFrameState`.

- [ ] **Step 1: Write failing graph-authority tests**

  Use a gimmick chart with BPM, negative scroll, and stop changes plus 105 accepted judgement samples. Assert: source-ordered chart series; per-second note totals and judgement/early-late buckets; gauge history; the most recent 100 timing samples in ring order; reset at attempt start; and identical snapshots for live/replay-driven input sequences.

- [ ] **Step 2: Run focused authority tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target gameplay_simulation_tests playfield_visual_state_tests play_skin_state_bridge_tests -j 6`

  Expected: failures because the current visual state has no complete graph authority and retains only 96 built-in indicator samples.

- [ ] **Step 3: Implement bounded source-shaped state**

  Define explicit value types and views:

  ```cpp
  struct SkinJudgeTimingSample {
    JudgeResult judge = JudgeResult::None;
    int timingDifferenceMillis = 0;
    bool early = false;
    std::int64_t eventTimeMillis = 0;
  };

  struct SkinNoteDistributionBucket {
    int second = 0;
    int notes = 0;
    std::array<int, 6> judgements{};
    int early = 0;
    int late = 0;
  };

  struct SkinBpmGraphPoint {
    std::int64_t chartTimeMicros = 0;
    double bpm = 0.0;
    double scroll = 1.0;
    std::int64_t stopMicros = 0;
  };

  struct SkinGameplayGraphState {
    std::vector<SkinNoteDistributionBucket> noteDistribution;
    std::vector<SkinBpmGraphPoint> bpmSeries;
    std::vector<float> gaugeHistory;
    std::array<SkinJudgeTimingSample, 100> recentTiming{};
    std::size_t recentTimingSize = 0;
    std::size_t recentTimingHead = 0;
  };
  ```

  Derive immutable chart series once in `PlayfieldChartVisualModel`; update accepted-judgement and gauge state in simulation/presentation authority. Expose ordered spans through `ISkinFrameState` without copying in the renderer.

- [ ] **Step 4: Run focused authority tests**

  Run: `cmake --build cmake-build-debug --target gameplay_simulation_tests playfield_visual_state_tests play_skin_state_bridge_tests -j 6 && ./cmake-build-debug/gameplay_simulation_tests && ./cmake-build-debug/playfield_visual_state_tests && ./cmake-build-debug/play_skin_state_bridge_tests`

  Expected: all executables pass, including 100-entry wraparound and replay equivalence.

- [ ] **Step 5: Commit the graph authority**

  Run: `git add src/scene/play/SkinGameplayGraphState.h src/scene/play/SkinGameplayGraphState.cpp src/scene/play/PlayfieldVisualState.h src/scene/play/PlayfieldVisualState.cpp src/scene/play/PlayfieldChartVisualModel.h src/scene/play/PlayfieldChartVisualModel.cpp src/scene/play/GameplaySimulation.h src/scene/play/GameplaySimulation.cpp src/skin/beatoraja/Skin2DRenderer.h src/skin/beatoraja/PlaySkinStateBridge.h src/skin/beatoraja/PlaySkinStateBridge.cpp tests/playfield_visual_state_tests.cpp tests/gameplay_simulation_tests.cpp tests/play_skin_state_bridge_tests.cpp && git commit -m "feat: publish gameplay skin graph authority"`

### Task 2: Judgement and note-distribution graph

**Files:**

- Modify: `src/skin/beatoraja/BeatorajaSkinModel.h`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.cpp`
- Create: `src/skin/beatoraja/SkinNoteDistributionGraphRenderer.h`
- Create: `src/skin/beatoraja/SkinNoteDistributionGraphRenderer.cpp`
- Modify: `src/skin/beatoraja/SkinModelValidator.cpp`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `tests/beatoraja_skin_model_tests.cpp`
- Modify: `tests/skin_draw_command_tests.cpp`
- Modify: `tests/fixtures/beatoraja_skin/lua/model/all_v1_objects.luaskin`

**Interfaces:**

- Consumes: `SkinNoteDistributionBucket` spans and pinned `SkinNoteDistributionGraph` configuration.
- Produces: typed `SkinNoteDistributionGraphObject` and deterministic primitive commands for normal, judgement, and early/late modes, including the Lua negative-type distribution form.

- [ ] **Step 1: Write failing decode/draw tests**

  Cover type `0` notes, type `1` judge, type `2` early/late, negative generic distribution selection, `backTexOff`, `delay`, `orderReverse`, `noGap`, and `noGapX`. Assert grid placement, color/order, delayed reveal, current-position cursor, and empty-chart behavior against `SkinNoteDistributionGraph.java`.

- [ ] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target beatoraja_skin_model_tests skin_draw_command_tests -j 6`

  Expected: decoder reports `skin_lua_model_judgegraph_unsupported` or distribution-graph unsupported.

- [ ] **Step 3: Implement typed decode and rendering**

  ```cpp
  enum class SkinNoteDistributionGraphType : std::uint8_t {
    Normal, Judge, EarlyLate
  };

  struct SkinNoteDistributionGraphObject {
    SkinNoteDistributionGraphType type;
    bool backgroundTextureOff = false;
    int delayMillis = 500;
    bool reverseOrder = false;
    bool noGap = false;
    bool noHorizontalGap = false;
  };
  ```

  Add the payload to `SkinObjectPayload`, decode Lua fields with pinned defaults, validate the mode, and append bounded solid-quad/line-strip commands in the dedicated renderer.

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target beatoraja_skin_model_tests skin_draw_command_tests -j 6 && ./cmake-build-debug/beatoraja_skin_model_tests && ./cmake-build-debug/skin_draw_command_tests`

  Expected: both pass with no judge/distribution unsupported diagnostic.

- [ ] **Step 5: Commit the graph family**

  Run: `git add src/skin/CMakeLists.txt src/skin/beatoraja/BeatorajaSkinModel.h src/skin/beatoraja/LuaSkinTableDecoder.cpp src/skin/beatoraja/SkinNoteDistributionGraphRenderer.h src/skin/beatoraja/SkinNoteDistributionGraphRenderer.cpp src/skin/beatoraja/SkinModelValidator.cpp src/skin/beatoraja/Skin2DRenderer.cpp tests/beatoraja_skin_model_tests.cpp tests/skin_draw_command_tests.cpp tests/fixtures/beatoraja_skin/lua/model/all_v1_objects.luaskin && git commit -m "feat: render Beatoraja note distribution graphs"`

### Task 3: Timing visualizer

**Files:**

- Modify: `src/skin/beatoraja/BeatorajaSkinModel.h`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.cpp`
- Create: `src/skin/beatoraja/SkinTimingVisualizerRenderer.h`
- Create: `src/skin/beatoraja/SkinTimingVisualizerRenderer.cpp`
- Modify: `src/skin/beatoraja/SkinModelValidator.cpp`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `tests/beatoraja_skin_model_tests.cpp`
- Modify: `tests/skin_draw_command_tests.cpp`

**Interfaces:**

- Consumes: recent timing samples and active judge windows.
- Produces: `SkinTimingVisualizerObject` and the pinned background bands, center line, and recent timing lines.

- [ ] **Step 1: Write failing timing-visualizer tests**

  Test defaults and custom `width`, `judgeWidthMillis`, `lineWidth`, center/judge colors, `transparent`, and `drawDecay`. Feed more than 100 samples and assert visible line position, judgement band selection, alpha decay, and ring ordering from `SkinTimingVisualizer.java`.

- [ ] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target beatoraja_skin_model_tests skin_draw_command_tests -j 6`

  Expected: `skin_lua_model_timingvisualizer_unsupported` is reported.

- [ ] **Step 3: Implement the timing visualizer**

  Add a typed payload containing all pinned colors and flags. Decode exact `JsonSkin.TimingVisualizer` defaults in Lua's equivalent table form. Render bands before lines, map `[-judgeWidthMillis,+judgeWidthMillis]` to the authored width, and use bounded primitive commands.

  ```cpp
  struct SkinTimingVisualizerObject {
    int width = 301;
    int judgeWidthMillis = 150;
    int lineWidth = 1;
    std::array<std::uint32_t, 5> judgeRgba{};
    std::uint32_t lineRgba = 0x00ff00ffU;
    std::uint32_t centerRgba = 0xffffffffU;
    bool transparent = false;
    bool drawDecay = true;
  };
  ```

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target beatoraja_skin_model_tests skin_draw_command_tests -j 6 && ./cmake-build-debug/beatoraja_skin_model_tests && ./cmake-build-debug/skin_draw_command_tests`

  Expected: both pass and the timing visualizer retains exactly the latest 100 samples.

- [ ] **Step 5: Commit the visualizer**

  Run: `git add src/skin/CMakeLists.txt src/skin/beatoraja/BeatorajaSkinModel.h src/skin/beatoraja/LuaSkinTableDecoder.cpp src/skin/beatoraja/SkinTimingVisualizerRenderer.h src/skin/beatoraja/SkinTimingVisualizerRenderer.cpp src/skin/beatoraja/SkinModelValidator.cpp src/skin/beatoraja/Skin2DRenderer.cpp tests/beatoraja_skin_model_tests.cpp tests/skin_draw_command_tests.cpp && git commit -m "feat: render Beatoraja timing visualizer"`

### Task 4: Hit-error visualizer

**Files:**

- Modify: `src/skin/beatoraja/BeatorajaSkinModel.h`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.cpp`
- Create: `src/skin/beatoraja/SkinHitErrorVisualizerRenderer.h`
- Create: `src/skin/beatoraja/SkinHitErrorVisualizerRenderer.cpp`
- Modify: `src/skin/beatoraja/SkinModelValidator.cpp`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `tests/beatoraja_skin_model_tests.cpp`
- Modify: `tests/skin_draw_command_tests.cpp`

**Interfaces:**

- Consumes: the same 100-entry timing ring and pinned hit-error configuration.
- Produces: hit lines, center line, optional EMA, color modes, transparency, and decay matching `SkinHitErrorVisualizer`.

- [ ] **Step 1: Write failing hit-error tests**

  Cover every `colorMode`, `hiterrorMode`, and `emaMode`, plus `alpha`, `windowLength`, `transparent`, `drawDecay`, custom colors, width, judge range, and line width. Assert EMA seed/window behavior and decay at the exact source sample ages.

- [ ] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target beatoraja_skin_model_tests skin_draw_command_tests -j 6`

  Expected: `skin_lua_model_hiterrorvisualizer_unsupported` is reported.

- [ ] **Step 3: Implement hit-error geometry**

  Add a typed payload with the pinned defaults from `JsonSkin.HitErrorVisualizer`; calculate EMA and draw eligibility from the immutable sample ring without retaining renderer state. Emit center/EMA/hit lines in pinned order and enforce the primitive budget before appending.

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target beatoraja_skin_model_tests skin_draw_command_tests -j 6 && ./cmake-build-debug/beatoraja_skin_model_tests && ./cmake-build-debug/skin_draw_command_tests`

  Expected: both pass for every mode combination.

- [ ] **Step 5: Commit the visualizer**

  Run: `git add src/skin/CMakeLists.txt src/skin/beatoraja/BeatorajaSkinModel.h src/skin/beatoraja/LuaSkinTableDecoder.cpp src/skin/beatoraja/SkinHitErrorVisualizerRenderer.h src/skin/beatoraja/SkinHitErrorVisualizerRenderer.cpp src/skin/beatoraja/SkinModelValidator.cpp src/skin/beatoraja/Skin2DRenderer.cpp tests/beatoraja_skin_model_tests.cpp tests/skin_draw_command_tests.cpp && git commit -m "feat: render Beatoraja hit error visualizer"`

### Task 5: Gameplay timing-distribution source no-op

**Files:**

- Modify: `src/skin/beatoraja/BeatorajaSkinModel.h`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.cpp`
- Modify: `src/skin/beatoraja/SkinModelValidator.cpp`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `tests/beatoraja_skin_model_tests.cpp`
- Modify: `tests/skin_draw_command_tests.cpp`

**Interfaces:**

- Consumes: a valid gameplay `timingdistributiongraph` declaration.
- Produces: a typed source-no-op object which validates without an unsupported diagnostic and emits no gameplay draw command, matching `SkinTimingDistributionGraph.prepare()` outside `MusicResult`.

- [ ] **Step 1: Write failing gameplay-state tests**

  Decode all `width`, `lineWidth`, color, `drawAverage`, and `drawDev` fields and assert they are retained for provenance. Then evaluate in gameplay and assert zero commands and zero unsupported diagnostics. Pin the test to `SkinTimingDistributionGraph.prepare()`, which sets `draw = false` unless state is `MusicResult`; result-screen rendering remains out of scope.

- [ ] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target beatoraja_skin_model_tests skin_draw_command_tests -j 6`

  Expected: `skin_lua_model_timingdistributiongraph_unsupported` is reported instead of the source-defined gameplay no-op.

- [ ] **Step 3: Implement the explicit typed no-op**

  Add `SkinTimingDistributionGraphObject`, decode the pinned defaults, validate its fields, and handle it explicitly in the gameplay renderer without emitting commands. Do not use `SkinBlankObject`, because that denotes an unsupported implementation fallback rather than an upstream gameplay-state no-op. Mark the ledger row `source-defined-noop` with `SkinTimingDistributionGraph.prepare` as its source symbol.

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target beatoraja_skin_model_tests skin_draw_command_tests -j 6 && ./cmake-build-debug/beatoraja_skin_model_tests && ./cmake-build-debug/skin_draw_command_tests`

  Expected: both pass with the declaration accepted and intentionally invisible during gameplay.

- [ ] **Step 5: Commit the gameplay source rule**

  Run: `git add src/skin/beatoraja/BeatorajaSkinModel.h src/skin/beatoraja/LuaSkinTableDecoder.cpp src/skin/beatoraja/SkinModelValidator.cpp src/skin/beatoraja/Skin2DRenderer.cpp tests/beatoraja_skin_model_tests.cpp tests/skin_draw_command_tests.cpp && git commit -m "fix: match gameplay timing distribution no-op"`

### Task 6: BPM/scroll/stop graph

**Files:**

- Modify: `src/skin/beatoraja/BeatorajaSkinModel.h`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.cpp`
- Create: `src/skin/beatoraja/SkinBpmGraphRenderer.h`
- Create: `src/skin/beatoraja/SkinBpmGraphRenderer.cpp`
- Modify: `src/skin/beatoraja/SkinModelValidator.cpp`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `tests/beatoraja_skin_model_tests.cpp`
- Modify: `tests/skin_draw_command_tests.cpp`

**Interfaces:**

- Consumes: immutable BPM/scroll/stop series and main/min/max BPM authority.
- Produces: `SkinBpmGraphObject` and source-colored log-scale graph geometry.

- [ ] **Step 1: Write failing BPM graph tests**

  Use constant BPM, min/main/max changes, scroll multipliers, stop intervals, and transitions. Assert the pinned `1/8..8` logarithmic range relative to main BPM, delay, line width, main/min/max/other colors, stop lines, transition lines, and zero/invalid input handling from `SkinBPMGraph.java`.

- [ ] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target beatoraja_skin_model_tests skin_draw_command_tests -j 6`

  Expected: `skin_lua_model_bpmgraph_unsupported` is reported.

- [ ] **Step 3: Implement BPM graph projection**

  Add a typed payload with `delayMillis`, `lineWidth`, and six pinned colors. Project `bpm * scroll`, stop markers, and transitions using source time/order, and emit deterministic line strips with no per-frame chart scan.

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target beatoraja_skin_model_tests skin_draw_command_tests -j 6 && ./cmake-build-debug/beatoraja_skin_model_tests && ./cmake-build-debug/skin_draw_command_tests`

  Expected: both pass for constant and gimmick fixtures.

- [ ] **Step 5: Commit the graph**

  Run: `git add src/skin/CMakeLists.txt src/skin/beatoraja/BeatorajaSkinModel.h src/skin/beatoraja/LuaSkinTableDecoder.cpp src/skin/beatoraja/SkinBpmGraphRenderer.h src/skin/beatoraja/SkinBpmGraphRenderer.cpp src/skin/beatoraja/SkinModelValidator.cpp src/skin/beatoraja/Skin2DRenderer.cpp tests/beatoraja_skin_model_tests.cpp tests/skin_draw_command_tests.cpp && git commit -m "feat: render Beatoraja BPM graphs"`

### Task 7: Gauge-history graph

**Files:**

- Modify: `src/skin/beatoraja/BeatorajaSkinModel.h`
- Modify: `src/skin/beatoraja/LuaSkinTableDecoder.cpp`
- Create: `src/skin/beatoraja/SkinGaugeGraphRenderer.h`
- Create: `src/skin/beatoraja/SkinGaugeGraphRenderer.cpp`
- Modify: `src/skin/beatoraja/SkinModelValidator.cpp`
- Modify: `src/skin/beatoraja/Skin2DRenderer.cpp`
- Modify: `src/skin/CMakeLists.txt`
- Modify: `tests/beatoraja_skin_model_tests.cpp`
- Modify: `tests/skin_draw_command_tests.cpp`

**Interfaces:**

- Consumes: immutable gauge history, gauge type, border, and the full `JsonSkin.GaugeGraph` color configuration.
- Produces: background regions, clear/border lines, and gauge history matching `SkinGaugeGraphObject` when used by a gameplay skin.

- [ ] **Step 1: Write failing gauge-graph tests**

  Cover all source gauge categories, below/above-border histories, default/custom color arrays, the fixed 1,500 ms reveal and 2 px line width, and empty/partial histories. Assert background, border, progressive reveal, graph command ordering, and exact source color selection.

- [ ] **Step 2: Run focused tests to verify they fail**

  Run: `cmake --build cmake-build-debug --target beatoraja_skin_model_tests skin_draw_command_tests -j 6`

  Expected: `skin_lua_model_gaugegraph_unsupported` is reported.

- [ ] **Step 3: Implement gauge-history drawing**

  Add `SkinGaugeGraphObject` with the pinned default palette and optional authored color matrix. Render from the published history only, clamp/project exactly where the Java object does, and emit bounded background and line primitives.

- [ ] **Step 4: Run focused tests**

  Run: `cmake --build cmake-build-debug --target beatoraja_skin_model_tests skin_draw_command_tests -j 6 && ./cmake-build-debug/beatoraja_skin_model_tests && ./cmake-build-debug/skin_draw_command_tests`

  Expected: both pass with no gaugegraph unsupported diagnostic.

- [ ] **Step 5: Commit the graph**

  Run: `git add src/skin/CMakeLists.txt src/skin/beatoraja/BeatorajaSkinModel.h src/skin/beatoraja/LuaSkinTableDecoder.cpp src/skin/beatoraja/SkinGaugeGraphRenderer.h src/skin/beatoraja/SkinGaugeGraphRenderer.cpp src/skin/beatoraja/SkinModelValidator.cpp src/skin/beatoraja/Skin2DRenderer.cpp tests/beatoraja_skin_model_tests.cpp tests/skin_draw_command_tests.cpp && git commit -m "feat: render Beatoraja gauge history graphs"`
