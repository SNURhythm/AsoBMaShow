# Gimmick Chart Render Traversal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render timelines that leave and later re-enter the visible lane under negative or extreme `#SCROLL` and huge BPM values while preserving current note lifecycle behavior.

**Architecture:** Extend the pure gameplay scroll geometry boundary with suffix extrema, visible scroll ranges, and rectangle visibility. `BMSRenderer` will cache suffix extrema beside timeline scroll positions, traverse non-monotonic timelines until the suffix proves no re-entry is possible, and cull only geometry outside the viewport.

**Tech Stack:** C++23, CMake/CTest, existing `BMSRenderer`, bgfx batch renderers, and the supplied `_00_strange_labyrinth.bms` as an external acceptance input.

## Global Constraints

- Alive normal and long notes must remain renderable after crossing the judge line for as long as their existing renderer lifecycle keeps them alive.
- Measure lines must remain hidden after their timing passes the judge line.
- Landmine and invisible-note expiration behavior must remain unchanged.
- Keep `latePoorTiming`, `BMSRendererState::orphanLongNotes`, and `longNoteLookaheadScratch` authoritative.
- Keep microsecond render timing and the existing `scrollPositionAtTime` interpolation.
- Do not restore millisecond render-time quantization.
- Do not cherry-pick PR #36; port only its non-monotonic traversal and viewport-culling concepts.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`.
- Do not add the supplied third-party BMS file to the repository.

---

### Task 1: Define the non-monotonic traversal contract

**Files:**
- Modify: `src/scene/play/GameplayScrollGeometry.h`
- Modify: `tests/gameplay_scroll_geometry_tests.cpp`

**Interfaces:**
- Produces: `gameplay_scroll_geometry::ScrollRange { double minimum; double maximum; }`
- Produces: `gameplay_scroll_geometry::ScrollSuffixExtrema { std::vector<double> minimum; std::vector<double> maximum; }`
- Produces: `buildScrollSuffixExtrema(std::span<const double>) -> ScrollSuffixExtrema`
- Produces: `visibleScrollRange(double, float, float, float, float, float) -> ScrollRange`
- Produces: `suffixCanReachVisibleRange(std::span<const double>, std::span<const double>, size_t, ScrollRange) -> bool`
- Produces: `suffixCanReachVisibleRange(const ScrollSuffixExtrema &, size_t, ScrollRange) -> bool`
- Produces: `noteRectangleIntersectsViewport(float, float, float, float) -> bool`

- [ ] **Step 1: Write the failing regression tests**

Add these includes to `tests/gameplay_scroll_geometry_tests.cpp`:

```cpp
#include <vector>
```

Add a double overload beside the existing `requireNear` helper:

```cpp
void requireNear(double actual, double expected, const char *message) {
  require(std::fabs(actual - expected) < 0.0000001, message);
}
```

Append the following assertions before `return 0`:

```cpp
  const std::vector<double> reentryPositions{
      0.0, 20'000.0, -10'000.0, 0.5, 50'000.0};
  const ScrollSuffixExtrema reentrySuffix =
      buildScrollSuffixExtrema(reentryPositions);
  require(reentrySuffix.minimum.size() == reentryPositions.size(),
          "suffix minima cover every timeline");
  require(reentrySuffix.maximum.size() == reentryPositions.size(),
          "suffix maxima cover every timeline");
  requireNear(reentrySuffix.minimum[1], -10'000.0,
              "suffix minima retain later negative scroll re-entry");
  requireNear(reentrySuffix.maximum[1], 50'000.0,
              "suffix maxima retain later huge positive scroll");
  requireNear(reentrySuffix.minimum[3], 0.5,
              "suffix minima narrow after the negative excursion");

  const ScrollRange visible =
      visibleScrollRange(0.0, 1.0F, -1.0F, 10.0F, 1.0F, 0.0F);
  requireNear(visible.minimum, -2.0,
              "visible scroll range includes a partially visible note below");
  requireNear(visible.maximum, 10.0,
              "visible scroll range ends at the upper viewport bound");
  require(renderY(reentryPositions[1], 0.0, 1.0F, 0.0F) > 10.0F,
          "the first extreme timeline is above the viewport");
  require(suffixCanReachVisibleRange(reentrySuffix, 1, visible),
          "an offscreen timeline cannot stop a later visible re-entry");
  require(!suffixCanReachVisibleRange(reentrySuffix, 4, visible),
          "traversal stops when the remaining suffix cannot re-enter");

  const std::vector<double> negativeReentryPositions{0.0, -20'000.0, 0.25};
  const ScrollSuffixExtrema negativeReentrySuffix =
      buildScrollSuffixExtrema(negativeReentryPositions);
  require(suffixCanReachVisibleRange(negativeReentrySuffix, 1, visible),
          "a negative excursion cannot stop a later visible re-entry");

  require(noteRectangleIntersectsViewport(-1.5F, 1.0F, -1.0F, 10.0F),
          "a note crossing the lower viewport bound remains visible");
  require(!noteRectangleIntersectsViewport(-2.1F, 1.0F, -1.0F, 10.0F),
          "a note entirely below the viewport is culled");
  require(!noteRectangleIntersectsViewport(10.1F, 1.0F, -1.0F, 10.0F),
          "a note entirely above the viewport is culled");
  require(noteRectangleIntersectsViewport(-0.5F, 1.0F, -1.0F, 10.0F),
          "crossing the judge line does not hide a normal note");
```

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests -j 6
```

Expected: compilation fails because `ScrollRange`, `ScrollSuffixExtrema`, and
the new helper functions are not defined.

- [ ] **Step 3: Implement the minimal pure helpers**

Add these includes to `src/scene/play/GameplayScrollGeometry.h`:

```cpp
#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>
```

Add these definitions inside `namespace gameplay_scroll_geometry`, after
`renderY`:

```cpp
struct ScrollRange {
  double minimum = 0.0;
  double maximum = 0.0;
};

struct ScrollSuffixExtrema {
  std::vector<double> minimum;
  std::vector<double> maximum;
};

inline ScrollSuffixExtrema
buildScrollSuffixExtrema(std::span<const double> positions) {
  ScrollSuffixExtrema result;
  result.minimum.resize(positions.size());
  result.maximum.resize(positions.size());
  for (std::size_t i = positions.size(); i-- > 0;) {
    if (i + 1 == positions.size()) {
      result.minimum[i] = positions[i];
      result.maximum[i] = positions[i];
      continue;
    }
    result.minimum[i] = std::min(positions[i], result.minimum[i + 1]);
    result.maximum[i] = std::max(positions[i], result.maximum[i + 1]);
  }
  return result;
}

inline ScrollRange visibleScrollRange(double currentScrollPosition, float rxhs,
                                      float lowerBound, float upperBound,
                                      float noteHeight, float judgeY) {
  if (rxhs <= 0.0F) {
    return {-std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity()};
  }
  const double first =
      currentScrollPosition +
      static_cast<double>(lowerBound - judgeY - noteHeight) /
          static_cast<double>(rxhs);
  const double last =
      currentScrollPosition +
      static_cast<double>(upperBound - judgeY) / static_cast<double>(rxhs);
  return {std::min(first, last), std::max(first, last)};
}

inline bool suffixCanReachVisibleRange(std::span<const double> suffixMinimum,
                                       std::span<const double> suffixMaximum,
                                       std::size_t timelineIndex,
                                       ScrollRange visible) {
  return timelineIndex < suffixMinimum.size() &&
         timelineIndex < suffixMaximum.size() &&
         suffixMinimum[timelineIndex] <= visible.maximum &&
         suffixMaximum[timelineIndex] >= visible.minimum;
}

inline bool suffixCanReachVisibleRange(const ScrollSuffixExtrema &suffix,
                                       std::size_t timelineIndex,
                                       ScrollRange visible) {
  return suffixCanReachVisibleRange(suffix.minimum, suffix.maximum,
                                    timelineIndex, visible);
}

inline bool noteRectangleIntersectsViewport(float y, float noteHeight,
                                            float lowerBound,
                                            float upperBound) {
  return y + noteHeight >= lowerBound && y <= upperBound;
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
git commit -m "test: define non-monotonic scroll traversal"
```

---

### Task 2: Wire suffix reachability into `BMSRenderer`

**Files:**
- Modify: `src/scene/play/BMSRenderer.h:194-197`
- Modify: `src/scene/play/BMSRenderer.cpp:1882-1933`
- Modify: `src/scene/play/BMSRenderer.cpp:2087-2163`
- Modify: `src/scene/play/BMSRenderer.cpp:2418-2560`

**Interfaces:**
- Consumes: all pure interfaces produced by Task 1.
- Preserves: `latePoorTiming`, `orphanLongNotes`, grouped note processing,
  invisible-note expiration, landmine expiration, and measure-line timing.
- Produces: `timelineScrollSuffixMin` and `timelineScrollSuffixMax` caches with
  the same length and indexing as `timelineScrollPositions`.

- [ ] **Step 1: Add suffix caches and build them with timeline positions**

Add the following members immediately after `timelineScrollPositions` in
`BMSRenderer.h`:

```cpp
  std::vector<double> timelineScrollSuffixMin;
  std::vector<double> timelineScrollSuffixMax;
```

At the beginning of `buildTimelineScrollPositions`, clear all three caches:

```cpp
  timelineScrollPositions.clear();
  timelineScrollSuffixMin.clear();
  timelineScrollSuffixMax.clear();
```

After the loop that fills `timelineScrollPositions`, add:

```cpp
  auto suffix = gameplay_scroll_geometry::buildScrollSuffixExtrema(
      timelineScrollPositions);
  timelineScrollSuffixMin = std::move(suffix.minimum);
  timelineScrollSuffixMax = std::move(suffix.maximum);
```

- [ ] **Step 2: Reuse the tested visible range for replay items**

In both `drawReplayGhosts` and `drawReplayMissMarkers`, replace the duplicated
`firstVisibleScrollPosition` / `lastVisibleScrollPosition` calculation and swap
with:

```cpp
  const auto visible = gameplay_scroll_geometry::visibleScrollRange(
      currentScrollPosition, rxhs, lowerBound, upperBound, noteRenderHeight,
      judgeY);
  const double firstVisibleScrollPosition = visible.minimum;
  const double lastVisibleScrollPosition = visible.maximum;
```

Do not change replay event filtering, timing, or rendering.

- [ ] **Step 3: Replace the monotonic early exit with suffix reachability**

Immediately after `currentScrollPosition` is calculated in `render`, add:

```cpp
  const auto visibleScrollRange =
      gameplay_scroll_geometry::visibleScrollRange(
          currentScrollPosition, rxhs, lowerBound, upperBound,
          noteRenderHeight, judgeY);
```

Replace the loop header with:

```cpp
  for (size_t i = state.currentTimelineIndex; i < timelines.size(); ++i) {
```

After obtaining `timeLine` and checking `timelineScrollPositions`, add:

```cpp
    const bool timelineIsFuture = timeLine->Timing >= micro;
    if (timelineIsFuture && longNoteLookahead.empty() &&
        !gameplay_scroll_geometry::suffixCanReachVisibleRange(
            timelineScrollSuffixMin, timelineScrollSuffixMax, i,
            visibleScrollRange)) {
      break;
    }
```

Keep the direct `renderY` call and all lifecycle branches below it unchanged.
The early exit is restricted to future timelines so old invisible notes still
reach their existing expiration branch and the lifecycle cursor can advance.

- [ ] **Step 4: Cull note rectangles only at viewport boundaries**

Change `drawNormalNote` to begin with:

```cpp
  if (note->IsPlayed ||
      !gameplay_scroll_geometry::noteRectangleIntersectsViewport(
          y, noteRenderHeight, lowerBound, upperBound))
    return;
```

Add the same helper condition to the existing played/dead guards in
`drawInvisibleNote` and `drawLandmineNote`:

```cpp
  if (note->IsPlayed || note->IsDead ||
      !gameplay_scroll_geometry::noteRectangleIntersectsViewport(
          y, noteRenderHeight, lowerBound, upperBound)) {
    return;
  }
```

Do not use `judgeY` as the lower boundary. Do not alter `drawLongNote`; its body
already submits geometry only when `bodyHeight > 0.0F`, and its modern
head/tail lifecycle must remain intact.

- [ ] **Step 5: Build and verify the renderer integration**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests main -j 6
ctest --test-dir cmake-build-debug --output-on-failure -R '^gameplay_scroll_geometry_tests$'
git diff --check
rg -n "i < timelines\.size\(\) && y < upperBound" src/scene/play/BMSRenderer.cpp
rg -n "latePoorTiming|orphanLongNotes|longNoteLookahead" src/scene/play/BMSRenderer.cpp src/scene/play/BMSRenderer.h
```

Expected: both targets build, the focused test passes, the diff check emits no
output, the old monotonic loop search returns no matches, and the lifecycle
search still finds all three mechanisms.

Commit:

```bash
git add src/scene/play/BMSRenderer.h src/scene/play/BMSRenderer.cpp src/scene/play/GameplayScrollGeometry.h
git commit -m "fix: render scroll gimmick re-entry"
```

---

### Task 3: Verify the complete branch and acceptance evidence

**Files:**
- Verify: `src/scene/play/GameplayScrollGeometry.h`
- Verify: `src/scene/play/BMSRenderer.h`
- Verify: `src/scene/play/BMSRenderer.cpp`
- Verify: `tests/gameplay_scroll_geometry_tests.cpp`
- Verify externally: `/tmp/codex-remote-attachments/019f6f95-220e-7662-87c5-7fdd9234a9db/453FC11F-23B6-4B0C-AB9B-DA437E4C9CA5/1-00_strange_labyrinth.bms`

**Interfaces:**
- Consumes: the complete implementation from Tasks 1 and 2.
- Produces: fresh build, focused-test, full-suite, source-fixture, and diff evidence.

- [ ] **Step 1: Confirm the external chart still exercises the protected extremes**

Run:

```bash
LC_ALL=C rg -n '^#BPM0[12] |^#SCROLL(1P|1S|1T) ' '/tmp/codex-remote-attachments/019f6f95-220e-7662-87c5-7fdd9234a9db/453FC11F-23B6-4B0C-AB9B-DA437E4C9CA5/1-00_strange_labyrinth.bms'
```

Expected: output includes BPM values `14500145` and `145000145`, plus scroll
values `-10000`, `20000`, and `10000`.

- [ ] **Step 2: Run fresh complete verification**

Run:

```bash
cmake --build cmake-build-debug --target main gameplay_scroll_geometry_tests -j 6
ctest --test-dir cmake-build-debug --output-on-failure -j 6
git diff d399e92^ --check
git status --short --branch
```

Expected: the build exits 0, all tests pass, the branch diff check emits no
output, and the working tree is clean after the implementation commits.
