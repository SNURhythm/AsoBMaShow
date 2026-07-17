# Beatoraja Render-Time Quantization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove high-BPM lane shake by sampling chart geometry at Beatoraja's whole-millisecond granularity without reducing gameplay or effect timing precision.

**Architecture:** Add a pure raw/geometry render-time split to the shared gameplay scroll helpers. Use the quantized time for every `BMSRenderer` chart traversal and geometry decision, while explicitly preserving the raw input for HUD and effect animation consumers.

**Tech Stack:** C++23, CMake/CTest, existing `BMSRenderer`, and the supplied `_00_strange_labyrinth.bms` diagnostic.

## Global Constraints

- Quantize only renderer-side chart geometry and visibility; do not change audio, judgement, or input clocks.
- Keep raw microseconds for pending HUD text, long-note texture animation, lane beams, and the judgement indicator.
- Keep `replayTouchTimeMicros` unchanged.
- Align replay ghost geometry with quantized note geometry.
- Match Java integer truncation toward zero during negative preroll.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`.
- Do not add the supplied third-party BMS file to the repository.
- Execute inline under the user's existing approval.

---

### Task 1: Define and apply the raw/geometry clock split

**Files:**
- Modify: `src/scene/play/GameplayScrollGeometry.h`
- Modify: `tests/gameplay_scroll_geometry_tests.cpp`
- Modify: `src/scene/play/BMSRenderer.cpp`

**Interfaces:**
- Produces: `RenderTimes { long long rawMicros; long long geometryMicros; }`
- Produces: `splitRenderTimes(long long) -> RenderTimes`
- Preserves: `BMSRenderer::render(RenderContext &, long long, long long)` and all public renderer interfaces.

- [ ] **Step 1: Write the failing render-time split tests**

Add near the start of `tests/gameplay_scroll_geometry_tests.cpp::main`:

```cpp
  const RenderTimes positiveTimes = splitRenderTimes(3'750'075);
  require(positiveTimes.rawMicros == 3'750'075,
          "the raw render clock keeps microsecond precision");
  require(positiveTimes.geometryMicros == 3'750'000,
          "chart geometry truncates to Beatoraja milliseconds");
  require(splitRenderTimes(3'750'999).geometryMicros == 3'750'000,
          "one millisecond uses one stable geometry sample");
  require(splitRenderTimes(-1'999).geometryMicros == -1'000,
          "negative preroll matches Java truncation toward zero");
  require(splitRenderTimes(4'000'000).geometryMicros == 4'000'000,
          "an exact millisecond is unchanged");
```

- [ ] **Step 2: Run the focused target and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests -j 6
```

Expected: compilation fails because `RenderTimes` and `splitRenderTimes` are
not defined.

- [ ] **Step 3: Implement the minimal pure split**

Add inside `namespace gameplay_scroll_geometry` in
`src/scene/play/GameplayScrollGeometry.h`:

```cpp
struct RenderTimes {
  long long rawMicros = 0;
  long long geometryMicros = 0;
};

inline RenderTimes splitRenderTimes(long long rawMicros) {
  return {rawMicros, (rawMicros / 1'000LL) * 1'000LL};
}
```

- [ ] **Step 4: Apply explicit clocks in `BMSRenderer`**

At the start of the three-argument `render`, create `renderTimes`,
`rawRenderTimeMicros`, and `geometryTimeMicros`. Replace chart timing and
geometry uses of the `micro` parameter with `geometryTimeMicros`, including
`scrollPositionAtTime`, timeline future/past classification, future Y,
measure-line visibility, renderer lifecycle thresholds, invisible/landmine
visibility, and replay ghost time.

Use `rawRenderTimeMicros` for:

```cpp
currentRenderMicros = rawRenderTimeMicros;
applyPendingHudText(rawRenderTimeMicros);
```

and for render-time lane beams and `judgementIndicator.render`. Leave
`replayTouchTimeMicros` untouched.

- [ ] **Step 5: Verify GREEN and regression coverage**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^gameplay_scroll_geometry_tests$'
cmake --build cmake-build-debug --target main -j 6
ctest --test-dir cmake-build-debug --output-on-failure
git diff --check
```

Expected: the focused target builds, all 80 tests pass, the desktop target
builds, and the diff check emits no output.

- [ ] **Step 6: Commit the correction**

```bash
git add src/scene/play/GameplayScrollGeometry.h \
  tests/gameplay_scroll_geometry_tests.cpp \
  src/scene/play/BMSRenderer.cpp
git commit -m "fix: stabilize gimmick chart geometry"
```
