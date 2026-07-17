# Shared Gameplay Scroll Geometry Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the post-judge-line constant-speed note workaround with the same chart-scroll geometry used by ghosts, share that transform with every timeline visual, and hide passed geometry while preserving active long-note bodies.

**Architecture:** Add a renderer-independent `GameplayScrollGeometry.h` policy with pure scroll-to-Y, visibility, visible-range, and active-long-note anchoring functions. `BMSRenderer` will precompute timeline scroll-position suffix bounds, place every timeline/replay visual through the shared policy, and use timing only as the lifecycle gate that prevents passed visuals from re-entering.

**Tech Stack:** C++23, CMake/CTest, existing `BMSRenderer` and bgfx batch renderers.

## Global Constraints

- Normal notes, long-note endpoints, measure lines, landmines, invisible notes, replay ghosts, and replay miss markers must use `judgeY + (itemScrollPosition - currentScrollPosition) * rxhs`.
- Timeline geometry is hidden after its timeline timing passes the current render time and whenever its transformed anchor is below the judge line or above the lane.
- An active long-note body whose head has passed remains anchored at the judge line until its tail passes.
- Remove the `latePoorTiming` rendering workaround; do not change judgement windows or note-state transitions.
- Do not change replay formats, chart parsing, lane-cover settings, or playback timing.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`.

---

### Task 1: Define and test the shared scroll geometry policy

**Files:**
- Create: `src/scene/play/GameplayScrollGeometry.h`
- Create: `tests/gameplay_scroll_geometry_tests.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `gameplay_scroll_geometry::renderY(double, double, float, float) -> float`
- Produces: `gameplay_scroll_geometry::hasPassed(long long, long long) -> bool`
- Produces: `gameplay_scroll_geometry::placeTimelineItem(...) -> TimelinePlacement`
- Produces: `gameplay_scroll_geometry::visibleScrollRange(...) -> ScrollRange`
- Produces: `gameplay_scroll_geometry::rangeIntersects(...) -> bool`
- Produces: `gameplay_scroll_geometry::activeLongNoteHeadY(...) -> float`

- [ ] **Step 1: Add the failing geometry regression test**

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
              "negative scroll distance maps below the judge line");

  const auto atLine = placeTimelineItem(10.0, 10.0, 2.0F, 0.5F, 8.5F,
                                        1'000'000, 1'000'000);
  requireNear(atLine.y, 0.5F, "timeline placement uses shared scroll Y");
  require(atLine.visible, "an item at the current timing remains visible");

  const auto passed = placeTimelineItem(10.0, 10.0, 2.0F, 0.5F, 8.5F,
                                        999'999, 1'000'000);
  require(!passed.visible, "a passed timeline item is hidden");

  const auto belowJudge = placeTimelineItem(9.0, 10.0, 2.0F, 0.5F, 8.5F,
                                            1'100'000, 1'000'000);
  require(!belowJudge.visible,
          "a future item below the judge-line anchor is hidden");

  const auto aboveLane = placeTimelineItem(15.0, 10.0, 2.0F, 0.5F, 8.5F,
                                           1'100'000, 1'000'000);
  require(!aboveLane.visible, "an item above the visible lane is hidden");

  const auto stoppedEarly = placeTimelineItem(12.0, 10.0, 2.0F, 0.5F, 8.5F,
                                              2'000'000, 1'000'000);
  const auto stoppedLater = placeTimelineItem(12.0, 10.0, 2.0F, 0.5F, 8.5F,
                                              2'000'000, 1'100'000);
  requireNear(stoppedEarly.y, stoppedLater.y,
              "elapsed time does not move geometry while scroll is stopped");

  requireNear(activeLongNoteHeadY(999'999, 1'000'000, -2.0F, 0.5F),
              0.5F, "a passed active long-note head anchors at the judge line");
  requireNear(activeLongNoteHeadY(1'000'000, 1'000'000, 0.5F, 0.5F),
              0.5F, "a current long-note head keeps its transformed Y");

  const auto range = visibleScrollRange(10.0, 2.0F, 0.5F, 8.5F);
  require(std::fabs(range.first - 10.0) < 0.0001 &&
              std::fabs(range.last - 14.0) < 0.0001,
          "visible lane converts back to a scroll-position range");
  require(rangeIntersects(9.0, 12.0, range),
          "a negative-scroll suffix that re-enters the lane remains eligible");
  require(!rangeIntersects(14.5, 20.0, range),
          "a suffix wholly above the visible range can be skipped");
  return 0;
}
```

Add this target beside `start_lane_indicator_geometry_tests` in `CMakeLists.txt`:

```cmake
    add_executable(gameplay_scroll_geometry_tests
        tests/gameplay_scroll_geometry_tests.cpp
    )
    target_include_directories(gameplay_scroll_geometry_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_scroll_geometry_tests PRIVATE cxx_std_23)
```

Add `gameplay_scroll_geometry_tests` to the `foreach(test_target IN ITEMS ...)` registration list immediately after `start_lane_indicator_geometry_tests`.

- [ ] **Step 2: Run the focused target and verify the RED state**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests -j 6
```

Expected: compilation fails because `scene/play/GameplayScrollGeometry.h` does not exist. Confirm the failure is the missing production interface, not a CMake or test syntax error.

- [ ] **Step 3: Implement the minimal shared geometry policy**

Create `src/scene/play/GameplayScrollGeometry.h` with:

```cpp
#pragma once

#include <algorithm>

namespace gameplay_scroll_geometry {

struct TimelinePlacement {
  float y = 0.0F;
  bool visible = false;
};

struct ScrollRange {
  double first = 0.0;
  double last = 0.0;
};

inline float renderY(double itemScrollPosition,
                     double currentScrollPosition, float rxhs,
                     float judgeY) {
  return judgeY +
         static_cast<float>(itemScrollPosition - currentScrollPosition) * rxhs;
}

inline bool hasPassed(long long itemTimeMicros,
                      long long currentTimeMicros) {
  return itemTimeMicros < currentTimeMicros;
}

inline bool isWithinVisibleLane(float y, float judgeY, float upperBound) {
  return y >= judgeY && y <= upperBound;
}

inline TimelinePlacement placeTimelineItem(
    double itemScrollPosition, double currentScrollPosition, float rxhs,
    float judgeY, float upperBound, long long itemTimeMicros,
    long long currentTimeMicros) {
  const float y =
      renderY(itemScrollPosition, currentScrollPosition, rxhs, judgeY);
  return {.y = y,
          .visible = !hasPassed(itemTimeMicros, currentTimeMicros) &&
                     isWithinVisibleLane(y, judgeY, upperBound)};
}

inline float activeLongNoteHeadY(long long headTimeMicros,
                                 long long currentTimeMicros,
                                 float transformedY, float judgeY) {
  return hasPassed(headTimeMicros, currentTimeMicros) ? judgeY : transformedY;
}

inline ScrollRange visibleScrollRange(double currentScrollPosition,
                                      float rxhs, float judgeY,
                                      float upperBound) {
  if (rxhs <= 0.0F) {
    return {.first = currentScrollPosition,
            .last = currentScrollPosition};
  }
  const double first = currentScrollPosition;
  const double last =
      currentScrollPosition +
      static_cast<double>(upperBound - judgeY) / static_cast<double>(rxhs);
  return {.first = std::min(first, last), .last = std::max(first, last)};
}

inline bool rangeIntersects(double minimum, double maximum,
                            const ScrollRange &visible) {
  return minimum <= visible.last && maximum >= visible.first;
}

} // namespace gameplay_scroll_geometry
```

- [ ] **Step 4: Run the focused geometry test and verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^gameplay_scroll_geometry_tests$'
```

Expected: the target builds and CTest reports `100% tests passed, 0 tests failed`.

- [ ] **Step 5: Commit the tested geometry boundary**

```bash
git add CMakeLists.txt src/scene/play/GameplayScrollGeometry.h tests/gameplay_scroll_geometry_tests.cpp
git commit -m "test: define shared gameplay scroll geometry"
```

---

### Task 2: Route every timeline and replay visual through the shared policy

**Files:**
- Modify: `src/scene/play/BMSRenderer.cpp`
- Modify: `src/scene/play/BMSRenderer.h`
- Test: `tests/gameplay_scroll_geometry_tests.cpp`

**Interfaces:**
- Consumes: every function in `gameplay_scroll_geometry` from Task 1.
- Produces: `BMSRenderer` timeline and replay geometry with one scroll transform and no `latePoorTiming` render state.

- [ ] **Step 1: Remove the constant-speed render dependency**

In `BMSRenderer.cpp`, add:

```cpp
#include "GameplayScrollGeometry.h"
```

Delete `kDefaultLatePoorTimingMicros`, `latePoorTimingFromWindows`, and the constructor initializer:

```cpp
latePoorTiming(latePoorTimingFromWindows(timingWindows)),
```

Delete this field from `BMSRenderer.h`:

```cpp
long long latePoorTiming;
```

The constructor keeps `timingWindows` because `judgementIndicator` still consumes it.

- [ ] **Step 2: Precompute safe suffix bounds for scroll re-entry**

Add these fields after `timelineScrollPositions` in `BMSRenderer.h`:

```cpp
std::vector<double> timelineScrollSuffixMin;
std::vector<double> timelineScrollSuffixMax;
```

Replace `buildTimelineScrollPositions()` with:

```cpp
void BMSRenderer::buildTimelineScrollPositions() {
  timelineScrollPositions.clear();
  timelineScrollSuffixMin.clear();
  timelineScrollSuffixMax.clear();
  timelineScrollPositions.reserve(timelines.size());
  if (timelines.empty()) {
    return;
  }

  double position = timelines.front()->BeatPosition;
  timelineScrollPositions.push_back(position);
  for (size_t i = 1; i < timelines.size(); ++i) {
    const auto *prevTimeline = timelines[i - 1];
    const auto *timeline = timelines[i];
    position += (timeline->BeatPosition - prevTimeline->BeatPosition) *
                prevTimeline->Scroll;
    timelineScrollPositions.push_back(position);
  }

  timelineScrollSuffixMin.resize(timelineScrollPositions.size());
  timelineScrollSuffixMax.resize(timelineScrollPositions.size());
  for (size_t i = timelineScrollPositions.size(); i-- > 0;) {
    const double current = timelineScrollPositions[i];
    if (i + 1 < timelineScrollPositions.size()) {
      timelineScrollSuffixMin[i] =
          std::min(current, timelineScrollSuffixMin[i + 1]);
      timelineScrollSuffixMax[i] =
          std::max(current, timelineScrollSuffixMax[i + 1]);
    } else {
      timelineScrollSuffixMin[i] = current;
      timelineScrollSuffixMax[i] = current;
    }
  }
}
```

- [ ] **Step 3: Share the transform and judge-line visibility with replay visuals**

In `drawReplayGhosts`, replace the hand-built visible bounds with:

```cpp
const auto visibleRange = gameplay_scroll_geometry::visibleScrollRange(
    currentScrollPosition, rxhs, judgeY, upperBound);
const double firstVisibleScrollPosition = visibleRange.first;
const double lastVisibleScrollPosition = visibleRange.last;
```

Replace the `ghostY` calculation with:

```cpp
const float ghostY = gameplay_scroll_geometry::renderY(
    event.judgeScrollPosition, currentScrollPosition, rxhs, judgeY);
```

Change `drawReplayMissMarkers` in both declaration and definition to accept
`long long currentTimeMicros` between `rxhs` and `currentScrollPosition`. Use
the same `visibleScrollRange` and `renderY` calls as ghosts, and add this first
inside its marker loop:

```cpp
if (gameplay_scroll_geometry::hasPassed(marker.noteTimeMicros,
                                        currentTimeMicros)) {
  continue;
}
```

Change the render call to:

```cpp
drawReplayMissMarkers(rxhs, micro, currentScrollPosition);
```

Replace the first visibility condition in both `drawGhostNoteOutline` and
`drawMissMarkerX` with:

```cpp
if (!gameplay_scroll_geometry::isWithinVisibleLane(y, judgeY, upperBound)) {
  return;
}
```

- [ ] **Step 4: Replace the timeline loop's incremental and late-window Y calculations**

Immediately after `currentScrollPosition` is calculated in `render`, initialize
the active long-note head positions and visible range with:

```cpp
auto &longNoteLookahead = longNoteLookaheadScratch;
longNoteLookahead.clear();
for (auto *orphanLongNote : state.orphanLongNotes) {
  longNoteLookahead[orphanLongNote] = judgeY;
}
const auto visibleScrollRange =
    gameplay_scroll_geometry::visibleScrollRange(
        currentScrollPosition, rxhs, judgeY, upperBound);
```

Use an unconditional index loop and begin each iteration with:

```cpp
for (size_t i = state.currentTimelineIndex; i < timelines.size(); ++i) {
  const auto *timeLine = timelines[i];
  if (i >= timelineScrollPositions.size() ||
      i >= timelineScrollSuffixMin.size() ||
      i >= timelineScrollSuffixMax.size()) {
    break;
  }

  const auto placement = gameplay_scroll_geometry::placeTimelineItem(
      timelineScrollPositions[i], currentScrollPosition, rxhs, judgeY,
      upperBound, timeLine->Timing, micro);
  y = placement.y;
  const bool timelinePassed =
      gameplay_scroll_geometry::hasPassed(timeLine->Timing, micro);

  if (!timelinePassed &&
      !gameplay_scroll_geometry::rangeIntersects(
          timelineScrollSuffixMin[i], timelineScrollSuffixMax[i],
          visibleScrollRange)) {
    break;
  }

  if (!timelinePassed && timeLine->IsFirstInMeasure && placement.visible) {
    drawRect(playAreaWidth, 0.05F, playAreaLeftX, y,
             Color(255, 255, 255, 128));
  } else if (timelinePassed) {
    state.currentTimelineIndex = i;
  }
```

Delete the old incremental future-timeline calculation and the entire
`micro - latePoorTiming` branch.

- [ ] **Step 5: Apply the placement lifecycle to notes, long notes, landmines, and invisible notes**

Within `processNote`, make `keepDeadLongNoteBody` retain bodies at the shared
active-head anchor:

```cpp
auto keepDeadLongNoteBody = [&]() {
  if (!note->IsLongNote()) {
    return false;
  }
  auto *longNote = static_cast<bms_parser::LongNote *>(note);
  if (!shouldKeepDeadLongNoteBody(longNote)) {
    return false;
  }
  state.orphanLongNotes.insert(longNote);
  longNoteLookahead[longNote] =
      gameplay_scroll_geometry::activeLongNoteHeadY(
          timeLine->Timing, micro, y, judgeY);
  return true;
};
```

Replace `if (timeLine->Timing >= micro - latePoorTiming)` with
`if (!timelinePassed)`. In that branch:

- draw normal notes only under `if (placement.visible)`;
- draw grouped landmines only under `if (placement.visible)`;
- store long-note heads only when `y >= judgeY`;
- draw a long-note tail only when `placement.visible`; if the matching head is
  absent, use `judgeY` as its body start; and
- if a future long-note tail has `y < judgeY`, erase its matching lookup entry
  without drawing it.

The resulting long-note dispatch is:

```cpp
if (note->IsLongNote()) {
  auto *longNote = static_cast<bms_parser::LongNote *>(note);
  if (longNote->IsTail()) {
    if (longNote->Head == nullptr) {
      return;
    }
    if (placement.visible) {
      if (auto it = longNoteLookahead.find(longNote->Head);
          it != longNoteLookahead.end()) {
        drawLongNote(it->second, y, longNote->Head);
        longNoteLookahead.erase(it);
      } else {
        drawLongNote(judgeY, y, longNote->Head);
      }
    } else if (y < judgeY) {
      longNoteLookahead.erase(longNote->Head);
    }
  } else if (y >= judgeY) {
    longNoteLookahead[longNote] = y;
  }
} else if (placement.visible) {
  drawNormalNote(y, note);
}
```

Keep the existing passed-long-note state transitions, but set a passed head's
lookahead value to `judgeY` instead of `lowerBound`. Normal notes and landmines
perform no draw in the passed branch.

Gate the separate invisible-note and landmine vectors with the same placement:

```cpp
if (!timelinePassed) {
  if (showInvisibleNotes && placement.visible) {
    drawInvisibleNote(y, note);
  }
} else {
  note->IsDead = true;
}
```

```cpp
if (!timelinePassed && placement.visible) {
  drawLandmineNote(y, note);
}
```

- [ ] **Step 6: Hide passed long-note endpoints while keeping the body anchored**

At the start of `drawLongNote`, after the existing tail-state booleans, add:

```cpp
const bool headPassed =
    head->Timeline != nullptr && gameplay_scroll_geometry::hasPassed(
                                     head->Timeline->Timing,
                                     currentRenderMicros);
```

Replace the `startY` calculation with:

```cpp
const float transformedHeadY =
    gameplay_scroll_geometry::activeLongNoteHeadY(
        head->Timeline != nullptr ? head->Timeline->Timing
                                  : currentRenderMicros,
        currentRenderMicros, headY, judgeY);
float startY = (head->IsPlayed && !head->IsDead) || headPassed
                   ? judgeY
                   : transformedHeadY;
```

Replace the final head guard with:

```cpp
if (head->IsPlayed || headPassed) {
  return;
}
```

This leaves the body and current/future tail drawing intact while suppressing
the already-passed head sprite.

- [ ] **Step 7: Run focused and compile verification**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^gameplay_scroll_geometry_tests$'
cmake --build cmake-build-debug --target main -j 6
git diff --check
```

Expected: focused tests report `100% tests passed`, the desktop `main` target
exits successfully, and `git diff --check` prints no errors.

Also run:

```bash
rg -n "latePoorTiming|micro - timeLine->Timing" src/scene/play/BMSRenderer.cpp src/scene/play/BMSRenderer.h
```

Expected: no matches.

- [ ] **Step 8: Commit the renderer integration**

```bash
git add src/scene/play/BMSRenderer.cpp src/scene/play/BMSRenderer.h
git commit -m "fix: share gameplay scroll geometry"
```
