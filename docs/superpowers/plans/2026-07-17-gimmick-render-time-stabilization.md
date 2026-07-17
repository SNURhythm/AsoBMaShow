# Gimmick Render-Time Stabilization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore whole-millisecond sampling for chart geometry without reducing the precision of non-chart renderer effects.

**Architecture:** Keep the existing `BMSRenderer` clock routing unchanged and implement stabilization entirely in `chartRenderTimeMicros`. The policy will truncate chart time to a millisecond while HUD, animation, judgement, beam, and replay-touch consumers continue receiving raw microseconds.

**Tech Stack:** C++23, CMake/CTest, existing gameplay scroll geometry tests, and the supplied `_00_strange_labyrinth.bms` diagnostic.

## Global Constraints

- Quantize only the chart-render policy to whole milliseconds.
- Preserve raw microseconds for HUD, animations, beams, judgement effects, and replay touches.
- Preserve bounded gimmick traversal and all current note-lifecycle behavior.
- Do not inspect or enforce lane-cover length or other chart setup.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`.
- Do not add the supplied third-party BMS file to the repository.
- Execute inline under the user's existing authorization.

---

### Task 1: Re-enable millisecond chart-time sampling

**Files:**
- Modify: `tests/gameplay_scroll_geometry_tests.cpp`
- Modify: `src/scene/play/GameplayScrollGeometry.h`

**Interfaces:**
- Preserves: `chartRenderTimeMicros(long long visualTimeMicros) -> long long`.
- Preserves: both `BMSRenderer::render` overloads and their current clock routing.

- [ ] **Step 1: Write the failing quantization assertions**

Replace the raw-time assertions at the start of
`tests/gameplay_scroll_geometry_tests.cpp::main` with:

```cpp
  require(chartRenderTimeMicros(3'750'075) == 3'750'000,
          "chart geometry truncates to whole milliseconds");
  require(chartRenderTimeMicros(3'750'999) == 3'750'000,
          "one millisecond uses one stable chart sample");
  require(chartRenderTimeMicros(-1'999) == -1'000,
          "negative preroll matches Java truncation toward zero");
  require(chartRenderTimeMicros(4'000'000) == 4'000'000,
          "an exact millisecond remains unchanged");
```

- [ ] **Step 2: Run the focused target and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests -j 6
./cmake-build-debug/gameplay_scroll_geometry_tests
```

Expected: the executable runs and exits with the message
`chart geometry truncates to whole milliseconds` because the current policy
returns `3'750'075` unchanged.

- [ ] **Step 3: Implement the minimal policy change**

Change `chartRenderTimeMicros` in
`src/scene/play/GameplayScrollGeometry.h` to:

```cpp
inline long long chartRenderTimeMicros(long long visualTimeMicros) {
  return (visualTimeMicros / 1'000LL) * 1'000LL;
}
```

- [ ] **Step 4: Verify GREEN and renderer integration**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^gameplay_scroll_geometry_tests$'
/tmp/asobmashow_shake_diagnostic /tmp/codex-remote-attachments/019f6f95-220e-7662-87c5-7fdd9234a9db/453FC11F-23B6-4B0C-AB9B-DA437E4C9CA5/1-00_strange_labyrinth.bms
ctest --test-dir cmake-build-debug --output-on-failure
git diff --check
```

Expected: the focused test and all 80 tests pass, the desktop target builds,
and the diagnostic reports the known stabilized-versus-raw sampling delta.

- [ ] **Step 5: Commit the stabilization**

```bash
git add src/scene/play/GameplayScrollGeometry.h \
  tests/gameplay_scroll_geometry_tests.cpp
git commit -m "fix: restore gimmick render stabilization"
```
