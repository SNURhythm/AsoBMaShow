# Chart Viewer Invisible-Note Outline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render enabled normal invisible notes as hollow orange outlines in the chart viewer while keeping invisible long notes solid.

**Architecture:** Add a pure header-only geometry helper beside the chart viewer that converts one invisible-note marker into either four border rectangles or one solid rectangle. `ChartViewerScene` will submit the returned rectangles through its existing clipped rectangle path, so zoom, scrolling, viewport clipping, color, and draw order remain unchanged.

**Tech Stack:** C++23, existing chart-viewer `SimpleBatchRenderer` path, CMake, CTest.

## Global Constraints

- Only enabled invisible normal-note markers in the chart viewer change appearance.
- Invisible long notes remain solid orange rectangles.
- Use a border thickness equal to 12 percent of marker height, clamped to half the marker width and height.
- Do not change gameplay rendering, parsing, settings, note ordering, expiration, or chart-viewer clipping and layering.
- Do not edit `src/bms_parser.hpp` or `src/bms_parser.cpp`.

## File Structure

- Create `src/scene/ChartViewerNoteGeometry.h`: pure rectangle geometry for chart-viewer invisible-note markers.
- Create `tests/chart_viewer_note_geometry_tests.cpp`: focused solid-versus-outline geometry regression tests.
- Modify `src/scene/ChartViewerScene.cpp`: consume the geometry helper from the existing invisible-note draw pass.
- Modify `CMakeLists.txt`: build and register the new lightweight test executable.

---

### Task 1: Invisible-note marker geometry and chart-viewer integration

**Files:**
- Create: `src/scene/ChartViewerNoteGeometry.h`
- Create: `tests/chart_viewer_note_geometry_tests.cpp`
- Modify: `src/scene/ChartViewerScene.cpp:1-2,1103-1121`
- Modify: `CMakeLists.txt:547-571,1783-1790`

**Interfaces:**
- Produces: `chart_viewer_note_geometry::Rectangle` with `x`, `y`, `width`, and `height` float members.
- Produces: `chart_viewer_note_geometry::Rectangles` with capacity for four rectangles and a `count` member.
- Produces: `chart_viewer_note_geometry::invisibleNoteRectangles(float x, float y, float width, float height, float borderThickness, bool isLongNote)`.
- Consumes: `ChartCanvasView::drawRectClip(float x, float y, float width, float height, uint32_t color)` for existing viewport clipping and batching.

- [ ] **Step 1: Add the failing geometry tests and test target**

Create `tests/chart_viewer_note_geometry_tests.cpp`:

```cpp
#include "scene/ChartViewerNoteGeometry.h"

#include <cmath>
#include <cstddef>
#include <iostream>

namespace {

bool near(float actual, float expected) {
  return std::fabs(actual - expected) < 0.0001F;
}

} // namespace

int main() {
  using namespace chart_viewer_note_geometry;
  int failures = 0;
  const auto check = [&](bool value, const char *label) {
    if (!value) {
      std::cerr << "FAIL: " << label << '\n';
      ++failures;
    }
  };

  const auto normal =
      invisibleNoteRectangles(10.0F, 20.0F, 8.0F, 6.0F, 0.72F, false);
  check(normal.count == 4,
        "a normal invisible note produces four border rectangles");
  bool centerCovered = false;
  for (std::size_t i = 0; i < normal.count; ++i) {
    const auto &rectangle = normal.rectangles[i];
    centerCovered =
        centerCovered ||
        (14.0F > rectangle.x &&
         14.0F < rectangle.x + rectangle.width &&
         23.0F > rectangle.y &&
         23.0F < rectangle.y + rectangle.height);
  }
  check(!centerCovered, "a normal invisible note leaves its center empty");

  const auto longNote =
      invisibleNoteRectangles(10.0F, 20.0F, 8.0F, 6.0F, 0.72F, true);
  check(longNote.count == 1,
        "an invisible long note produces one solid rectangle");
  check(near(longNote.rectangles[0].x, 10.0F) &&
            near(longNote.rectangles[0].y, 20.0F) &&
            near(longNote.rectangles[0].width, 8.0F) &&
            near(longNote.rectangles[0].height, 6.0F),
        "the invisible long-note rectangle covers the full marker");

  const auto narrow =
      invisibleNoteRectangles(0.0F, 0.0F, 0.5F, 6.0F, 0.72F, false);
  check(narrow.count == 4,
        "a narrow normal invisible note retains four border rectangles");
  check(near(narrow.rectangles[0].height, 0.25F) &&
            near(narrow.rectangles[2].width, 0.25F),
        "outline thickness is clamped to half the marker width");

  return failures == 0 ? 0 : 1;
}
```

In `CMakeLists.txt`, add the test target after
`lane_cover_number_geometry_tests`:

```cmake
    add_executable(chart_viewer_note_geometry_tests
        tests/chart_viewer_note_geometry_tests.cpp
    )
    target_include_directories(chart_viewer_note_geometry_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(chart_viewer_note_geometry_tests PRIVATE cxx_std_23)
```

Add it to the registered lightweight-test list after
`lane_cover_number_geometry_tests`:

```cmake
        lane_cover_number_geometry_tests
        chart_viewer_note_geometry_tests
        gameplay_scroll_geometry_tests
```

- [ ] **Step 2: Run the focused test target to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target chart_viewer_note_geometry_tests -j 6
```

Expected: compilation fails because
`scene/ChartViewerNoteGeometry.h` does not exist.

- [ ] **Step 3: Add the minimal chart-viewer geometry helper**

Create `src/scene/ChartViewerNoteGeometry.h`:

```cpp
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace chart_viewer_note_geometry {

struct Rectangle {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
};

struct Rectangles {
  std::array<Rectangle, 4> rectangles{};
  std::size_t count = 0;
};

inline Rectangles invisibleNoteRectangles(
    float x, float y, float width, float height, float borderThickness,
    bool isLongNote) {
  Rectangles result;
  if (width <= 0.0F || height <= 0.0F) {
    return result;
  }
  if (isLongNote) {
    result.rectangles[0] = {x, y, width, height};
    result.count = 1;
    return result;
  }

  const float border = std::min(
      {borderThickness, width * 0.5F, height * 0.5F});
  if (border <= 0.0F) {
    return result;
  }
  result.rectangles = {
      Rectangle{x, y, width, border},
      Rectangle{x, y + height - border, width, border},
      Rectangle{x, y, border, height},
      Rectangle{x + width - border, y, border, height}};
  result.count = result.rectangles.size();
  return result;
}

} // namespace chart_viewer_note_geometry
```

- [ ] **Step 4: Run the focused test to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target chart_viewer_note_geometry_tests -j 6
./cmake-build-debug/chart_viewer_note_geometry_tests
```

Expected: the target builds and the test executable exits with status 0 and
no output.

- [ ] **Step 5: Consume the helper in the chart viewer**

Add the include immediately after `ChartViewerScene.h` in
`src/scene/ChartViewerScene.cpp`:

```cpp
#include "ChartViewerScene.h"
#include "ChartViewerNoteGeometry.h"
#include "ChartListenStart.h"
```

Replace the body of `drawInvisibleNotes()` with:

```cpp
  void drawInvisibleNotes() {
    if (!showInvisibleNotes) {
      return;
    }
    forEachInvisibleNote([&](int lane, const bms_parser::Note *note,
                             const bms_parser::TimeLine *timeline) {
      auto yIt = timelineY.find(timeline);
      auto layoutIt = timelineMeasure.find(timeline);
      if (yIt == timelineY.end() || layoutIt == timelineMeasure.end()) {
        return;
      }
      const auto &layout = measureLayouts[layoutIt->second];
      const float x = laneContentX(layout.column, lane) + 2.0F;
      const float y = yIt->second - 3.0F;
      const float width = std::max(5.0F, laneWidth - 4.0F);
      constexpr float height = 6.0F;
      const bool isLongNote =
          dynamic_cast<const bms_parser::LongNote *>(note) != nullptr;
      const auto rectangles =
          chart_viewer_note_geometry::invisibleNoteRectangles(
              x, y, width, height, height * 0.12F, isLongNote);
      for (std::size_t i = 0; i < rectangles.count; ++i) {
        const auto &rectangle = rectangles.rectangles[i];
        drawRectClip(rectangle.x, rectangle.y, rectangle.width,
                     rectangle.height, invisibleNoteColor());
      }
    });
  }
```

- [ ] **Step 6: Verify the focused test and desktop compilation**

Run:

```bash
cmake --build cmake-build-debug --target chart_viewer_note_geometry_tests main -j 6
./cmake-build-debug/chart_viewer_note_geometry_tests
git diff --check
```

Expected: both targets build, the focused test exits with status 0, and
`git diff --check` prints nothing.

- [ ] **Step 7: Commit the implementation**

```bash
git add \
  CMakeLists.txt \
  src/scene/ChartViewerNoteGeometry.h \
  src/scene/ChartViewerScene.cpp \
  tests/chart_viewer_note_geometry_tests.cpp
git commit -m "fix: outline invisible notes in chart viewer"
```

---

### Task 2: Full regression verification

**Files:**
- Verify only; no new files.

**Interfaces:**
- Consumes: the chart-viewer invisible-note geometry and integration from Task 1.
- Produces: fresh desktop build and test evidence for handoff.

- [ ] **Step 1: Verify the committed diff and desktop target**

Run:

```bash
git diff --check HEAD~1..HEAD
cmake --build cmake-build-debug --target main -j 6
```

Expected: diff checking prints nothing and the desktop target exits with
status 0.

- [ ] **Step 2: Run the complete test suite**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: all configured tests pass with zero failures, including
`chart_viewer_note_geometry_tests`.

- [ ] **Step 3: Confirm the repository state and implementation commit**

Run:

```bash
git status --short
git log -2 --oneline
```

Expected: the working tree is clean and the latest implementation commit is
`fix: outline invisible notes in chart viewer`.
