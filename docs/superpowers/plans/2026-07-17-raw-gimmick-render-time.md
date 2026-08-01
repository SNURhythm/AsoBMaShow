# Raw Gimmick Render-Time Restoration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore correct Labyrinth content by preserving the renderer's sub-millisecond chart clock instead of quantizing traversal to whole milliseconds.

**Architecture:** Replace the raw/quantized `RenderTimes` split with a single policy helper that returns visual chart time unchanged. `BMSRenderer` will use that exact chart time for geometry, traversal, lifecycle visibility, and replay geometry while keeping the existing independent replay-touch clock.

**Tech Stack:** C++23, CMake/CTest, existing `BMSRenderer`, and the supplied `_00_strange_labyrinth.bms` diagnostics.

## Global Constraints

- Restore one internally consistent raw microsecond clock for chart geometry and visibility.
- Do not change audio, input, judgement, parser, replay formats, or note lifecycle rules.
- Keep `replayTouchTimeMicros` independent.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`.
- Do not add the supplied third-party BMS file to the repository.
- Remove the superseded millisecond-quantization design and plan.
- Do not implement a replacement anti-shake algorithm in this correction.
- Execute inline under the user's existing authorization.

---

### Task 1: Restore the raw chart-render clock contract

**Files:**
- Modify: `src/scene/play/GameplayScrollGeometry.h`
- Modify: `tests/gameplay_scroll_geometry_tests.cpp`
- Modify: `src/scene/play/BMSRenderer.cpp`
- Delete: `docs/superpowers/specs/2026-07-17-beatoraja-render-time-quantization-design.md`
- Delete: `docs/superpowers/plans/2026-07-17-beatoraja-render-time-quantization.md`

**Interfaces:**
- Removes: `RenderTimes` and `splitRenderTimes(long long)`.
- Produces: `chartRenderTimeMicros(long long visualTimeMicros) -> long long`.
- Preserves: both `BMSRenderer::render` overloads and the independent `replayTouchTimeMicros` argument.

- [ ] **Step 1: Write the failing raw-time contract test**

Replace the `RenderTimes` assertions at the start of
`tests/gameplay_scroll_geometry_tests.cpp::main` with:

```cpp
  require(chartRenderTimeMicros(3'750'075) == 3'750'075,
          "chart traversal preserves sub-millisecond visual time");
  require(chartRenderTimeMicros(3'750'999) == 3'750'999,
          "chart traversal does not collapse one millisecond of content");
  require(chartRenderTimeMicros(-1'999) == -1'999,
          "negative preroll preserves sub-millisecond visual time");
  require(chartRenderTimeMicros(4'000'000) == 4'000'000,
          "an exact millisecond remains unchanged");
```

- [ ] **Step 2: Run the focused target and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests -j 6
```

Expected: compilation fails because `chartRenderTimeMicros` is not defined.

- [ ] **Step 3: Implement the minimal raw-time policy**

Replace `RenderTimes` and `splitRenderTimes` in
`src/scene/play/GameplayScrollGeometry.h` with:

```cpp
inline long long chartRenderTimeMicros(long long visualTimeMicros) {
  return visualTimeMicros;
}
```

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^gameplay_scroll_geometry_tests$'
```

Expected: the focused test builds and passes.

- [ ] **Step 4: Route renderer chart consumers through the raw policy**

At the start of the three-argument `BMSRenderer::render`, replace the
`RenderTimes` split with:

```cpp
  const long long chartTimeMicros =
      gameplay_scroll_geometry::chartRenderTimeMicros(micro);
  currentRenderMicros = micro;
  applyPendingHudText(micro);
```

Replace each `geometryTimeMicros` use with `chartTimeMicros`. Use `micro`
directly for render-time lane beams and `judgementIndicator.render`. Leave
`replayTouchTimeMicros` unchanged.

- [ ] **Step 5: Remove superseded quantization documentation**

Delete:

```text
docs/superpowers/specs/2026-07-17-beatoraja-render-time-quantization-design.md
docs/superpowers/plans/2026-07-17-beatoraja-render-time-quantization.md
```

The committed raw-time restoration design and this plan remain authoritative.

- [ ] **Step 6: Verify the correction**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^gameplay_scroll_geometry_tests$'
/tmp/asobmashow_shake_diagnostic /tmp/codex-remote-attachments/019f6f95-220e-7662-87c5-7fdd9234a9db/453FC11F-23B6-4B0C-AB9B-DA437E4C9CA5/1-00_strange_labyrinth.bms
ctest --test-dir cmake-build-debug --output-on-failure
git diff --check
git status --short
```

Expected: the focused test and all 80 tests pass, the desktop target builds,
the diagnostic still demonstrates the removed quantized path's content delta,
and the diff check emits no output.

- [ ] **Step 7: Commit the correction**

```bash
git add src/scene/play/GameplayScrollGeometry.h \
  tests/gameplay_scroll_geometry_tests.cpp \
  src/scene/play/BMSRenderer.cpp \
  docs/superpowers/specs/2026-07-17-beatoraja-render-time-quantization-design.md \
  docs/superpowers/plans/2026-07-17-beatoraja-render-time-quantization.md \
  docs/superpowers/plans/2026-07-17-raw-gimmick-render-time.md
git commit -m "fix: restore raw gimmick render timing"
```
