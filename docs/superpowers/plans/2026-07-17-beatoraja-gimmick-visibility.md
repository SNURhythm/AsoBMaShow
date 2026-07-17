# Beatoraja-Compatible Gimmick Visibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the labyrinth chart's overdraw by matching Beatoraja's retained, bounded future-timeline traversal while preserving AsoBMaShow's absolute chart-scroll geometry for alive past notes.

**Architecture:** Replace suffix reachability with three pure compatibility helpers: timeline retention, incremental future Y, and the direct `y <= upperBound` continuation rule. `BMSRenderer` will filter BGA-only rows during construction, use absolute positions for late past rows, and use the retained predecessor formula only for future rows.

**Tech Stack:** C++23, CMake/CTest, existing `BMSRenderer`, bgfx batch renderers, and the supplied `_00_strange_labyrinth.bms` as an external acceptance input.

## Global Constraints

- Alive normal and long notes must remain renderable after crossing the judge line while their existing lifecycle keeps them alive.
- Measure lines must remain hidden after their timing passes the judge line.
- Landmine and invisible-note expiration behavior must remain unchanged.
- Keep `latePoorTiming`, `BMSRendererState::orphanLongNotes`, and `longNoteLookaheadScratch` authoritative.
- Keep microsecond render timing and the existing absolute `scrollPositionAtTime` path for past notes and replay markers.
- Match Beatoraja's retained timeline sequence and first-top-exit behavior for future chart content.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`.
- Do not add the supplied third-party BMS file to the repository.
- Do not spawn subagents; execute this plan inline as authorized by the user.

---

### Task 1: Define the compatibility traversal contract

**Files:**
- Modify: `src/scene/play/GameplayScrollGeometry.h`
- Modify: `tests/gameplay_scroll_geometry_tests.cpp`

**Interfaces:**
- Produces: `shouldKeepRenderTimeline(double, double, double, double, double, bool, bool, bool, bool) -> bool`
- Produces: `initialFutureTimelineY(double, double, long long, long long, double) -> double`
- Produces: `advanceFutureTimelineY(double, double, double, long long, double, long long, long long, double) -> double`
- Produces: `futureTimelineTraversalContinues(double, float) -> bool`
- Removes: suffix-extrema construction and suffix-reachability traversal.
- Preserves: `renderY`, `visibleScrollRange`, `noteRectangleIntersectsViewport`, and `shouldDrawMeasureLine`.

- [ ] **Step 1: Write the failing compatibility tests**

In `tests/gameplay_scroll_geometry_tests.cpp`, remove the suffix-extrema and
re-entry assertions. Add these assertions after the measure-line tests:

```cpp
  require(!shouldKeepRenderTimeline(145.0, 145.0, 0.0, 1.0, 1.0,
                                    false, false, false, false),
          "a BGA-only row is omitted from render traversal");
  require(shouldKeepRenderTimeline(145.0, 290.0, 0.0, 1.0, 1.0,
                                   false, false, false, false),
          "a BPM change remains in render traversal");
  require(shouldKeepRenderTimeline(145.0, 145.0, 0.0, 1.0, -10'000.0,
                                   false, false, false, false),
          "a scroll change remains in render traversal");
  require(shouldKeepRenderTimeline(145.0, 145.0, 0.0, 1.0, 1.0,
                                   false, true, false, false),
          "a playable note remains in render traversal");
  require(shouldKeepRenderTimeline(145.0, 145.0, 0.0, 1.0, 1.0,
                                   false, false, true, false),
          "an invisible note remains in render traversal");
  require(shouldKeepRenderTimeline(145.0, 145.0, 0.0, 1.0, 1.0,
                                   false, false, false, true),
          "a landmine remains in render traversal");

  requireNear(initialFutureTimelineY(0.5, 0.0, 0, 0, 16.0), 0.5,
              "the zero-time chart origin starts at the judge line");
  requireNear(advanceFutureTimelineY(0.5, 1.0, 1.0, 0, 0.0,
                                     1'000, 500, 10.0),
              5.5,
              "future Y advances by remaining segment travel");
  requireNear(advanceFutureTimelineY(0.5, 1.0, 1.0, 0, 1'000.0,
                                     2'000, 500, 10.0),
              10.5,
              "an active stop uses the full section distance");

  const double collapsedY = advanceFutureTimelineY(
      0.5, 1.0, 20'000.0, 1'000, 0.0, 1'000, 1'000, 10.0);
  require(std::isnan(collapsedY),
          "a huge-BPM zero-duration pair forms a traversal boundary");
  require(futureTimelineTraversalContinues(10.0, 10.0F),
          "a row exactly at the lane top is processed");
  require(!futureTimelineTraversalContinues(10.1, 10.0F),
          "the first row above the lane top ends traversal");
  require(!futureTimelineTraversalContinues(collapsedY, 10.0F),
          "a non-finite collapsed row ends traversal");
  require(futureTimelineTraversalContinues(
              -std::numeric_limits<double>::infinity(), 10.0F),
          "the continuation rule preserves direct comparison semantics");
```

Add `#include <limits>` and remove `#include <vector>`.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests -j 6
```

Expected: compilation fails because the four new compatibility helpers are
not defined.

- [ ] **Step 3: Implement the minimal pure helpers**

In `src/scene/play/GameplayScrollGeometry.h`, remove `ScrollSuffixExtrema`,
`buildScrollSuffixExtrema`, both `suffixCanReachVisibleRange` overloads, and
`shouldStopTimelineTraversal`. Remove unused `<cstddef>`, `<span>`, and
`<vector>` includes. Keep `<algorithm>` and `<limits>`.

Add after `renderY`:

```cpp
inline bool shouldKeepRenderTimeline(
    double previousBpm, double bpm, double stopDurationMicros,
    double previousScroll, double scroll, bool isMeasureLine,
    bool hasPlayableNote, bool hasInvisibleNote, bool hasLandmine) {
  return previousBpm != bpm || stopDurationMicros > 0.0 ||
         previousScroll != scroll || isMeasureLine || hasPlayableNote ||
         hasInvisibleNote || hasLandmine;
}

inline double initialFutureTimelineY(double currentY, double beatPosition,
                                     long long timelineTimeMicros,
                                     long long currentTimeMicros,
                                     double rxhs) {
  if (timelineTimeMicros == 0) {
    return currentY;
  }
  return currentY + beatPosition *
                        static_cast<double>(timelineTimeMicros -
                                            currentTimeMicros) /
                        static_cast<double>(timelineTimeMicros) * rxhs;
}

inline double advanceFutureTimelineY(
    double currentY, double beatDistance, double previousScroll,
    long long previousTimeMicros, double previousStopDurationMicros,
    long long timelineTimeMicros, long long currentTimeMicros, double rxhs) {
  if (static_cast<double>(previousTimeMicros) +
          previousStopDurationMicros >
      static_cast<double>(currentTimeMicros)) {
    return currentY + beatDistance * previousScroll * rxhs;
  }
  const double travelDuration =
      static_cast<double>(timelineTimeMicros - previousTimeMicros) -
      previousStopDurationMicros;
  if (travelDuration == 0.0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return currentY + beatDistance * previousScroll *
                        static_cast<double>(timelineTimeMicros -
                                            currentTimeMicros) /
                        travelDuration * rxhs;
}

inline bool futureTimelineTraversalContinues(double y, float upperBound) {
  return y <= static_cast<double>(upperBound);
}
```

- [ ] **Step 4: Verify GREEN and commit the pure contract**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^gameplay_scroll_geometry_tests$'
git diff --check
```

Expected: the target builds, the focused test passes, and the diff check emits
no output.

Commit:

```bash
git add src/scene/play/GameplayScrollGeometry.h tests/gameplay_scroll_geometry_tests.cpp
git commit -m "test: define bounded gimmick traversal"
```

---

### Task 2: Apply retained bounded traversal in `BMSRenderer`

**Files:**
- Modify: `src/scene/play/BMSRenderer.h`
- Modify: `src/scene/play/BMSRenderer.cpp`

**Interfaces:**
- Consumes: all four pure interfaces from Task 1.
- Preserves: absolute `timelineScrollPositions`, `scrollPositionAtTime`,
  `latePoorTiming`, `orphanLongNotes`, grouped note processing, invisible-note
  expiration, landmine expiration, and replay marker geometry.
- Removes: `timelineScrollSuffixMin` and `timelineScrollSuffixMax` caches.

- [ ] **Step 1: Filter render timelines while keeping note groups aligned**

In the constructor, build each row's temporary grouped-note vector before
deciding whether to retain it. Track `previousBpm` and `previousScroll` across
every model row. Use:

```cpp
      const bool hasInvisibleNote =
          std::any_of(timeLine->InvisibleNotes.begin(),
                      timeLine->InvisibleNotes.end(),
                      [](const auto *note) { return note != nullptr; });
      const bool hasLandmine =
          std::any_of(timeLine->LandmineNotes.begin(),
                      timeLine->LandmineNotes.end(),
                      [](const auto *note) { return note != nullptr; });
      const bool keep = gameplay_scroll_geometry::shouldKeepRenderTimeline(
          previousBpm, timeLine->Bpm, timeLine->GetStopDuration(),
          previousScroll, timeLine->Scroll, timeLine->IsFirstInMeasure,
          !timelineNotes.empty(), hasInvisibleNote, hasLandmine);
      previousBpm = timeLine->Bpm;
      previousScroll = timeLine->Scroll;
      if (!keep) {
        continue;
      }
      timelines.push_back(timeLine);
      groupedTimelineNotes.push_back(std::move(timelineNotes));
```

Initialize the tracked values to `chart->Meta.Bpm` and `1.0`.

- [ ] **Step 2: Remove suffix caches**

Delete `timelineScrollSuffixMin` and `timelineScrollSuffixMax` from
`BMSRenderer.h`. In `buildTimelineScrollPositions`, remove their clears and
the call to `buildScrollSuffixExtrema`; keep only absolute position building.

- [ ] **Step 3: Split past and future Y in the render loop**

Remove the render-local visible scroll range and suffix stop guard. Before the
timeline loop, add:

```cpp
  double futureY = static_cast<double>(judgeY);
  bool futureTraversalStarted = false;
```

At the beginning of each iteration, after `timelineIsFuture` is known, add:

```cpp
    if (timelineIsFuture && futureTraversalStarted &&
        !gameplay_scroll_geometry::futureTimelineTraversalContinues(
            futureY, upperBound)) {
      break;
    }

    if (timelineIsFuture) {
      if (i > 0) {
        const auto *previous = timelines[i - 1];
        futureY = gameplay_scroll_geometry::advanceFutureTimelineY(
            futureY, timeLine->BeatPosition - previous->BeatPosition,
            previous->Scroll, previous->Timing,
            previous->GetStopDuration(), timeLine->Timing, micro,
            static_cast<double>(rxhs));
      } else {
        futureY = gameplay_scroll_geometry::initialFutureTimelineY(
            futureY, timeLine->BeatPosition, timeLine->Timing, micro,
            static_cast<double>(rxhs));
      }
      y = static_cast<float>(futureY);
      futureTraversalStarted = true;
    } else {
      y = gameplay_scroll_geometry::renderY(
          timelineScrollPositions[i], currentScrollPosition, rxhs, judgeY);
    }
```

Keep the measure-line test and all timing/lifecycle branches after this block.
At the start of `processNote`, after the null guard, add:

```cpp
      if (timelineIsFuture && !std::isfinite(y)) {
        return;
      }
```

This prevents non-finite long-note geometry from entering batch state; normal,
invisible, and landmine rectangles are already rejected by viewport checks.

- [ ] **Step 4: Build and run the focused verification**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^gameplay_scroll_geometry_tests$'
git diff --check
rg -n "timelineScrollSuffix|suffixCanReach|shouldStopTimelineTraversal" src tests
```

Expected: both targets build, the focused test passes, the diff check emits no
output, and the removed suffix search returns no matches.

Commit:

```bash
git add src/scene/play/BMSRenderer.h src/scene/play/BMSRenderer.cpp
git commit -m "fix: bound gimmick chart visibility"
```

---

### Task 3: Validate the supplied chart and full repository

**Files:**
- No repository file changes expected.

**Interfaces:**
- Validates: retained-row count and zero-duration boundaries on the supplied
  chart, focused traversal behavior, the desktop build, and all registered
  tests.

- [ ] **Step 1: Parse the supplied chart with the temporary diagnostic**

Run the existing temporary diagnostic against:

```text
/tmp/codex-remote-attachments/019f6f95-220e-7662-87c5-7fdd9234a9db/453FC11F-23B6-4B0C-AB9B-DA437E4C9CA5/1-00_strange_labyrinth.bms
```

Expected retention evidence: 23,348 total rows, 23,109 retained rows, 239
dropped rows, 238 BGA-only dropped rows, and 18,732 zero-duration retained
pairs.

- [ ] **Step 2: Run fresh full verification**

Run:

```bash
cmake --build cmake-build-debug --target main gameplay_scroll_geometry_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure
git diff --check
git status --short
```

Expected: both targets build, all tests pass with zero failures, the diff check
emits no output, and the worktree contains no uncommitted repository changes.
