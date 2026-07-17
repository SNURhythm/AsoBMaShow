# Shared Gameplay Scroll Geometry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make normal and long notes use replay-ghost scroll geometry after crossing the judge line without hiding them, and make measure lines share that geometry while disappearing after they pass.

**Architecture:** Add a small renderer-independent geometry helper containing the shared scroll-to-Y transform and the measure-line visibility rule. `BMSRenderer` computes every timeline Y directly from its cached scroll position; existing note, long-note, landmine, and invisible-note lifecycle code remains intact.

**Tech Stack:** C++23, CMake/CTest, existing `BMSRenderer` and bgfx batch renderers.

## Global Constraints

- Normal and long notes remain visible after crossing the judge line while their existing late-window/state lifecycle keeps them alive.
- Notes use `judgeY + (timelineScrollPosition - currentScrollPosition) * rxhs` on both sides of the judge line.
- Measure lines use the same transform and are hidden once their timeline timing is past or their anchor is outside the lane at or above the judge line.
- Landmines and invisible notes inherit the corrected timeline Y but keep their existing expiration logic unchanged.
- Replay ghosts and miss markers retain their lifecycle and visibility behavior; only their duplicated Y expression is routed through the helper.
- Keep `latePoorTiming` for note lifecycle; remove only its constant-speed Y interpolation.
- Do not change replay formats, judgement logic, chart parsing, lane-cover settings, or playback timing.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`.

---

### Task 1: Define the corrected shared geometry contract

**Files:**
- Create: `src/scene/play/GameplayScrollGeometry.h`
- Create: `tests/gameplay_scroll_geometry_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `gameplay_scroll_geometry::renderY(double, double, float, float) -> float`
- Produces: `gameplay_scroll_geometry::shouldDrawMeasureLine(long long, long long, float, float, float) -> bool`

- [ ] **Step 1: Write the failing regression test**

Create `tests/gameplay_scroll_geometry_tests.cpp` with:

```cpp
#include "scene/play/GameplayScrollGeometry.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void requireNear(float actual, float expected, const char *message) {
  require(std::fabs(actual - expected) < 0.0001F, message);
}
} // namespace

int main() {
  using namespace gameplay_scroll_geometry;

  requireNear(renderY(10.0, 10.0, 2.0F, 0.5F), 0.5F,
              "equal scroll positions map to the judge line");
  requireNear(renderY(12.0, 10.0, 2.0F, 0.5F), 4.5F,
              "positive scroll distance maps above the judge line");
  requireNear(renderY(8.0, 10.0, 2.0F, 0.5F), -3.5F,
              "a passed note keeps its chart-scroll position below the line");

  const float stoppedBefore = renderY(12.0, 10.0, 2.0F, 0.5F);
  const float stoppedAfter = renderY(12.0, 10.0, 2.0F, 0.5F);
  requireNear(stoppedBefore, stoppedAfter,
              "elapsed time cannot move a note while chart scroll is stopped");

  require(shouldDrawMeasureLine(1'000'000, 1'000'000, 0.5F, 0.5F,
                                8.5F),
          "a measure line at the current timing remains visible");
  require(!shouldDrawMeasureLine(999'999, 1'000'000, -0.5F, 0.5F,
                                 8.5F),
          "a passed measure line is hidden");
  require(!shouldDrawMeasureLine(1'100'000, 1'000'000, -0.5F, 0.5F,
                                 8.5F),
          "a future measure line below the judge line is hidden");
  require(!shouldDrawMeasureLine(1'100'000, 1'000'000, 9.0F, 0.5F,
                                 8.5F),
          "a future measure line above the visible lane is hidden");
  return 0;
}
```

Register `gameplay_scroll_geometry_tests` beside
`start_lane_indicator_geometry_tests` in `CMakeLists.txt`, include `src`, use
C++23, and add it to the existing `foreach(test_target IN ITEMS ...)` list.

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests -j 6
```

Expected: compilation fails because `GameplayScrollGeometry.h` or the corrected
`shouldDrawMeasureLine` interface is missing.

- [ ] **Step 3: Implement the minimal helper**

Create `src/scene/play/GameplayScrollGeometry.h` with:

```cpp
#pragma once

namespace gameplay_scroll_geometry {

inline float renderY(double itemScrollPosition,
                     double currentScrollPosition, float rxhs,
                     float judgeY) {
  return judgeY +
         static_cast<float>(itemScrollPosition - currentScrollPosition) * rxhs;
}

inline bool shouldDrawMeasureLine(long long timelineTimeMicros,
                                  long long currentTimeMicros, float y,
                                  float judgeY, float upperBound) {
  return timelineTimeMicros >= currentTimeMicros && y >= judgeY &&
         y <= upperBound;
}

} // namespace gameplay_scroll_geometry
```

- [ ] **Step 4: Verify GREEN and commit the tested boundary**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^gameplay_scroll_geometry_tests$'
```

Expected: `100% tests passed, 0 tests failed`.

Commit:

```bash
git add CMakeLists.txt src/scene/play/GameplayScrollGeometry.h tests/gameplay_scroll_geometry_tests.cpp
git commit -m "test: define gameplay scroll geometry"
```

---

### Task 2: Apply the transform without changing note expiration

**Files:**
- Modify: `src/scene/play/BMSRenderer.cpp`

**Interfaces:**
- Consumes: `gameplay_scroll_geometry::renderY` and `shouldDrawMeasureLine`.
- Preserves: `latePoorTiming`, `orphanLongNotes`, long-note state handling, landmine expiration, and invisible-note expiration.

- [ ] **Step 1: Include the shared helper and reuse it for replay Y**

Add:

```cpp
#include "GameplayScrollGeometry.h"
```

Replace only the `ghostY` and `markerY` expressions with:

```cpp
const float ghostY = gameplay_scroll_geometry::renderY(
    event.judgeScrollPosition, currentScrollPosition, rxhs, judgeY);
```

```cpp
const float markerY = gameplay_scroll_geometry::renderY(
    marker.noteScrollPosition, currentScrollPosition, rxhs, judgeY);
```

Do not change ghost or miss-marker filtering, bounds, or timing.

- [ ] **Step 2: Compute every timeline Y from cached chart scroll**

At the start of each timeline-loop iteration, after obtaining `timeLine`, add:

```cpp
if (i >= timelineScrollPositions.size()) {
  break;
}
y = gameplay_scroll_geometry::renderY(
    timelineScrollPositions[i], currentScrollPosition, rxhs, judgeY);
```

Delete the incremental future-timeline Y block and delete only this
constant-speed late-timeline assignment:

```cpp
y = judgeY + (micro - timeLine->Timing) /
                 static_cast<float>(latePoorTiming) * lowerBound;
```

Keep the surrounding `timeLine->Timing >= micro - latePoorTiming` condition so
normal and long notes continue rendering during the existing late window.

- [ ] **Step 3: Gate measure lines with the corrected lifecycle**

Replace the measure-line condition with:

```cpp
if (timeLine->IsFirstInMeasure &&
    gameplay_scroll_geometry::shouldDrawMeasureLine(
        timeLine->Timing, micro, y, judgeY, upperBound)) {
  drawRect(playAreaWidth, 0.05F, playAreaLeftX, y,
           Color(255, 255, 255, 128));
}
```

Leave the grouped-note dispatch and the separate invisible-note and landmine
loops unchanged. Because they already consume `y`, they inherit the shared
transform while preserving expiration.

- [ ] **Step 4: Verify behavior, compilation, and the absence of the workaround**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^gameplay_scroll_geometry_tests$'
cmake --build cmake-build-debug --target main -j 6
git diff --check
rg -n "\(micro - timeLine->Timing\).*latePoorTiming|latePoorTiming\) \* lowerBound" src/scene/play/BMSRenderer.cpp
```

Expected: focused tests pass, `main` builds, `git diff --check` is clean, and
the final search returns no matches. `latePoorTiming` itself must still exist.

- [ ] **Step 5: Commit the renderer change**

```bash
git add src/scene/play/BMSRenderer.cpp
git commit -m "fix: keep late notes on chart scroll geometry"
```

---

### Task 3: Final verification

**Files:**
- Verify: all files changed by Tasks 1 and 2

**Interfaces:**
- Consumes: the complete branch state.
- Produces: fresh evidence that the focused behavior, full test suite, and desktop build pass together.

- [ ] **Step 1: Run fresh complete verification**

```bash
cmake --build cmake-build-debug --target main gameplay_scroll_geometry_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6
git diff HEAD~2 --check
git status --short
```

Expected: build exits 0, all tests pass, the diff check is clean, and the
working tree is clean after the two implementation commits.
