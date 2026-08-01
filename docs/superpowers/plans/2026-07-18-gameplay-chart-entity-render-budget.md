# Gameplay Chart-Entity Render Budget Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bound gameplay chart and replay-world rendering to 60,000 rectangles per frame so pathological charts cannot exhaust bgfx resources and erase the HUD or application UI.

**Architecture:** Add a small bgfx-independent rectangle budget beside the gameplay renderer and unit-test its boundary and latch behavior. `BMSRenderer` owns one instance, resets it once per frame, charges chart entities during the existing chronological traversal, carries an atomic three-rectangle reservation with each long-note head, and lets replay ghosts consume only the capacity left after chart geometry.

**Tech Stack:** C++23, CMake/CTest, bgfx batch renderers, existing `BMSRenderer` row-order traversal.

## Global Constraints

- The exact per-frame cap is 60,000 rectangles.
- The budget covers measure lines, normal notes, long-note bodies/heads/tails, landmines, enabled invisible notes, replay ghost outlines, and replay miss-marker blocks.
- Chart entities consume capacity before replay ghosts and miss markers.
- A long note reserves three rectangles at its head row and carries that authorization through lookahead; unused capacity is not refunded.
- A normal invisible note reserves all of its visible outline segments atomically; an invisible long-note marker reserves one rectangle.
- A replay ghost reserves four rectangles atomically; a replay miss marker reserves fourteen rectangles atomically.
- A rejected atomic request consumes nothing and latches the budget exhausted until the next frame reset.
- Budget exhaustion must not stop timeline traversal, note expiration, orphan tracking, gameplay simulation, or replay-state updates.
- Lane background, judge line, lane beams, lane cover, start-lane indicators, judgement UI, gauge, HUD, touch visuals, and application UI remain outside this budget.
- Preserve existing timing, scroll geometry, note clipping, appearance, chart parsing, row-based ordering, replay contents, and bgfx pool sizes.

## File Map

- Create `src/scene/play/GameplayChartEntityRenderBudget.h`: define rectangle-cost constants and the frame-local, atomic-consume budget object without depending on bgfx.
- Create `tests/gameplay_chart_entity_render_budget_tests.cpp`: verify the cap, shape costs, exact-boundary acceptance, atomic rejection, exhaustion latch, and reset.
- Modify `CMakeLists.txt`: build and register the focused budget test.
- Modify `src/scene/play/BMSRenderer.h`: include the budget, own one instance, carry long-note reservation state, and extend the long-note draw contract.
- Modify `src/scene/play/BMSRenderer.cpp`: reset and consume the shared budget in every covered draw path while preserving traversal and submission order.

---

### Task 1: Add and Test the Frame-Local Rectangle Budget

**Files:**
- Create: `src/scene/play/GameplayChartEntityRenderBudget.h`
- Create: `tests/gameplay_chart_entity_render_budget_tests.cpp`
- Modify: `CMakeLists.txt:571-578`
- Modify: `CMakeLists.txt:1791-1800`

**Interfaces:**
- Consumes: only `<cstdint>` from the C++ standard library.
- Produces: `gameplay_chart_entity_render_budget::Budget`, `Budget::reset()`, `Budget::tryConsume(uint32_t)`, `Budget::remaining() const`, `Budget::exhausted() const`, and the exact rectangle-cost constants used by `BMSRenderer` in Task 2.

- [ ] **Step 1: Register the focused test executable before its source can compile**

Add this target immediately after `gameplay_scroll_geometry_tests` in `CMakeLists.txt`:

```cmake
    add_executable(gameplay_chart_entity_render_budget_tests
        tests/gameplay_chart_entity_render_budget_tests.cpp
    )
    target_include_directories(gameplay_chart_entity_render_budget_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )
    target_compile_features(gameplay_chart_entity_render_budget_tests PRIVATE
        cxx_std_23
    )
```

Add `gameplay_chart_entity_render_budget_tests` immediately after `gameplay_scroll_geometry_tests` in the `foreach(test_target IN ITEMS ...)` registration list.

- [ ] **Step 2: Write the failing budget test**

Create `tests/gameplay_chart_entity_render_budget_tests.cpp` with:

```cpp
#include "scene/play/GameplayChartEntityRenderBudget.h"

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
  using namespace gameplay_chart_entity_render_budget;

  require(kMaxRectanglesPerFrame == 60'000U,
          "gameplay chart rendering keeps the approved frame cap");
  require(kSingleRectangleEntityCost == 1U,
          "a simple chart entity costs one rectangle");
  require(kLongNoteReservationCost == 3U,
          "a long note reserves body, head, and tail");
  require(kReplayGhostOutlineCost == 4U,
          "a replay ghost reserves its complete outline");
  require(kReplayMissMarkerCost == 14U,
          "a replay miss marker reserves its complete X");

  Budget budget;
  require(budget.remaining() == kMaxRectanglesPerFrame,
          "a new frame starts with the complete budget");
  require(!budget.exhausted(), "a new frame is not exhausted");
  require(budget.tryConsume(kReplayGhostOutlineCost),
          "a complete atomic shape fits within the budget");
  require(budget.remaining() ==
              kMaxRectanglesPerFrame - kReplayGhostOutlineCost,
          "an accepted request consumes its complete cost");

  Budget exactBoundary;
  require(exactBoundary.tryConsume(kMaxRectanglesPerFrame),
          "an exact-boundary request is accepted");
  require(exactBoundary.remaining() == 0U,
          "an exact-boundary request consumes all capacity");
  require(exactBoundary.exhausted(),
          "zero remaining capacity reports exhausted");
  require(!exactBoundary.tryConsume(kSingleRectangleEntityCost),
          "no request is accepted after exact exhaustion");

  Budget rejected;
  require(rejected.tryConsume(kMaxRectanglesPerFrame - 2U),
          "setup leaves less room than a long-note reservation");
  require(!rejected.tryConsume(kLongNoteReservationCost),
          "an over-budget atomic request is rejected");
  require(rejected.remaining() == 2U,
          "a rejected request does not partially consume capacity");
  require(rejected.exhausted(),
          "the first rejected request latches exhaustion");
  require(!rejected.tryConsume(kSingleRectangleEntityCost),
          "later smaller shapes stay rejected after the latch");
  require(rejected.remaining() == 2U,
          "latched rejections leave the remainder unchanged");

  rejected.reset();
  require(rejected.remaining() == kMaxRectanglesPerFrame,
          "the next frame restores the complete budget");
  require(!rejected.exhausted(),
          "reset clears the exhaustion latch");
  require(rejected.tryConsume(kLongNoteReservationCost),
          "atomic requests work again after reset");

  return 0;
}
```

- [ ] **Step 3: Build the focused test and verify that it fails for the missing production header**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_chart_entity_render_budget_tests -j 6
```

Expected: compilation fails with `scene/play/GameplayChartEntityRenderBudget.h` not found. This proves the new test is exercising an interface that does not yet exist.

- [ ] **Step 4: Implement the minimal tested budget**

Create `src/scene/play/GameplayChartEntityRenderBudget.h` with:

```cpp
#pragma once

#include <cstdint>

namespace gameplay_chart_entity_render_budget {

inline constexpr uint32_t kMaxRectanglesPerFrame = 60'000U;
inline constexpr uint32_t kSingleRectangleEntityCost = 1U;
inline constexpr uint32_t kLongNoteReservationCost = 3U;
inline constexpr uint32_t kReplayGhostOutlineCost = 4U;
inline constexpr uint32_t kReplayMissMarkerCost = 14U;

class Budget {
public:
  [[nodiscard]] bool tryConsume(uint32_t rectangleCount) {
    if (isExhausted || rectangleCount > remainingRectangles) {
      isExhausted = true;
      return false;
    }
    remainingRectangles -= rectangleCount;
    isExhausted = remainingRectangles == 0U;
    return true;
  }

  void reset() {
    remainingRectangles = kMaxRectanglesPerFrame;
    isExhausted = false;
  }

  [[nodiscard]] uint32_t remaining() const { return remainingRectangles; }
  [[nodiscard]] bool exhausted() const { return isExhausted; }

private:
  uint32_t remainingRectangles = kMaxRectanglesPerFrame;
  bool isExhausted = false;
};

} // namespace gameplay_chart_entity_render_budget
```

- [ ] **Step 5: Build and run the focused test**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_chart_entity_render_budget_tests -j 6
./cmake-build-debug/gameplay_chart_entity_render_budget_tests
```

Expected: the target builds successfully and the executable exits with status 0 and no output.

- [ ] **Step 6: Commit the tested budget component**

```bash
git add CMakeLists.txt \
  src/scene/play/GameplayChartEntityRenderBudget.h \
  tests/gameplay_chart_entity_render_budget_tests.cpp
git commit -m "feat: add gameplay chart render budget"
```

---

### Task 2: Apply the Shared Budget to Gameplay Chart and Replay Geometry

**Files:**
- Modify: `src/scene/play/BMSRenderer.h:9-20,199-204,306-319`
- Modify: `src/scene/play/BMSRenderer.cpp:1842-2020,2188-2300,2480-2740`

**Interfaces:**
- Consumes: `gameplay_chart_entity_render_budget::Budget` and all cost constants created in Task 1.
- Produces: one shared `BMSRenderer::chartEntityRenderBudget`; `LongNoteLookahead::renderBudgetReserved`; and `drawLongNote(float, float, bms_parser::LongNote *const &, gameplay_note_submission_order::LongNoteOrder, bool)`.

- [ ] **Step 1: Add the budget ownership and long-note reservation contract**

In `src/scene/play/BMSRenderer.h`, include the new header beside the other gameplay helpers:

```cpp
#include "GameplayChartEntityRenderBudget.h"
#include "GameplayNoteSubmissionOrder.h"
```

Extend the lookahead entry and add the shared budget immediately after its scratch map:

```cpp
  struct LongNoteLookahead {
    float headY = 0.0F;
    gameplay_note_submission_order::LongNoteOrder order;
    bool renderBudgetReserved = false;
  };
  std::unordered_map<bms_parser::LongNote *, LongNoteLookahead>
      longNoteLookaheadScratch;
  gameplay_chart_entity_render_budget::Budget chartEntityRenderBudget;
```

Change the declaration of `drawLongNote` to require its carried authorization:

```cpp
  void drawLongNote(
      float headY, float tailY, bms_parser::LongNote *const &head,
      gameplay_note_submission_order::LongNoteOrder order,
      bool renderBudgetReserved);
```

- [ ] **Step 2: Make every deferred long note an all-or-nothing authorized shape**

Change the `drawLongNote` definition signature and return before any assertion, state inspection, batch lookup, or geometry submission when the head did not reserve capacity:

```cpp
void BMSRenderer::drawLongNote(
    float headY, float tailY, bms_parser::LongNote *const &head,
    gameplay_note_submission_order::LongNoteOrder order,
    bool renderBudgetReserved) {
  if (!renderBudgetReserved) {
    return;
  }
  // assert head
  assert(!head->IsTail() && "head is tail");
```

Leave the remaining long-note body, tail, and head drawing logic unchanged. Its single reservation is deliberately not refunded when clipping or note state causes fewer than three rectangles to be emitted.

- [ ] **Step 3: Charge normal notes and landmines after visibility checks but before batch or texture lookup**

In `drawNormalNote`, insert this block after the existing played/clip/viewport early return and before `sheetForLane`:

```cpp
  if (!chartEntityRenderBudget.tryConsume(
          gameplay_chart_entity_render_budget::kSingleRectangleEntityCost)) {
    return;
  }
```

Insert the identical block in `drawLandmineNote` after its existing played/dead/clip/viewport early return and before `sheetForLane`.

This placement means callers may calculate a numeric row depth, but rejected entities never call `noteTextureBatchAtDepth`, never create a batch transition, and never add geometry.

- [ ] **Step 4: Charge invisible notes atomically before selecting the invisible batch depth**

Replace the body of `drawInvisibleNote` after its existing visibility early return with:

```cpp
  const uint32_t color = Color(255, 149, 36, 224).toABGR();
  const float x = laneToX(note->Lane);
  if (note->IsLongNote()) {
    if (!chartEntityRenderBudget.tryConsume(
            gameplay_chart_entity_render_budget::
                kSingleRectangleEntityCost)) {
      return;
    }
    setInvisibleBatchDepth(submitDepth);
    gimmickBatchRenderer.addRect(x, clip.y, noteRenderWidth, clip.height,
                                 color);
    return;
  }

  const float borderThickness =
      std::max(0.015F,
               noteRenderHeight *
                   gameplay_scroll_geometry::kInvisibleNoteBorderHeightRatio);
  const auto outline = gameplay_scroll_geometry::noteOutlineRectangles(
      x, y, noteRenderWidth, noteRenderHeight, borderThickness, clip);
  if (outline.count == 0U ||
      !chartEntityRenderBudget.tryConsume(
          static_cast<uint32_t>(outline.count))) {
    return;
  }

  setInvisibleBatchDepth(submitDepth);
  for (std::size_t i = 0; i < outline.count; ++i) {
    const auto &rect = outline.rectangles[i];
    gimmickBatchRenderer.addRect(rect.x, rect.y, rect.width, rect.height,
                                 color);
  }
```

The outline count is computed from the already-clipped geometry, so a three-segment clipped outline reserves three and a complete outline reserves four. No segment is emitted unless the complete visible outline fits.

- [ ] **Step 5: Charge replay ghost shapes atomically**

In `drawGhostNoteOutline`, insert this block after the viewport early return and before color calculation:

```cpp
  if (!chartEntityRenderBudget.tryConsume(
          gameplay_chart_entity_render_budget::kReplayGhostOutlineCost)) {
    return;
  }
```

In `drawMissMarkerX`, replace the start of the function through the `kSteps` declaration with:

```cpp
void BMSRenderer::drawMissMarkerX(float y, const ReplayMissMarker &marker) {
  if (y + noteRenderHeight < lowerBound || y > upperBound) {
    return;
  }

  constexpr int kSteps = 7;
  static_assert(kSteps * 2 ==
                gameplay_chart_entity_render_budget::kReplayMissMarkerCost);
  if (!chartEntityRenderBudget.tryConsume(
          gameplay_chart_entity_render_budget::kReplayMissMarkerCost)) {
    return;
  }
```

Leave the existing seven-iteration, two-block-per-iteration X drawing loop unchanged. Because both replay draw passes remain after all chart traversal, they can use only chart capacity that remains.

- [ ] **Step 6: Reset the budget and charge visible measure lines**

At the beginning of `BMSRenderer::render`, place the reset immediately before the renderer begin calls:

```cpp
  simpleBatchRenderer.setSubmitView(rendering::main_view);
  simpleBatchRenderer.setSubmitDepth(kBackgroundDepth);
  gimmickBatchRenderer.setSubmitView(rendering::main_view);
  ghostBatchRenderer.setSubmitDepth(kGhostDepth);
  chartEntityRenderBudget.reset();
  simpleBatchRenderer.begin();
```

Extend the existing measure-line condition so its one rectangle is reserved before `drawRect`:

```cpp
    if (timeLine->IsFirstInMeasure &&
        gameplay_scroll_geometry::shouldDrawMeasureLine(
            timeLine->Timing, chartTimeMicros, y, judgeY, upperBound) &&
        chartEntityRenderBudget.tryConsume(
            gameplay_chart_entity_render_budget::
                kSingleRectangleEntityCost)) {
      drawRect(playAreaWidth, 0.05f, playAreaLeftX, y,
               Color(255, 255, 255, 128));
    }
```

Do not break the timeline loop when this returns false; expiration and lookahead state must continue to advance.

- [ ] **Step 7: Reserve long-note capacity once at each head and carry it through lookahead**

Replace the current lookahead initialization, from `auto &longNoteLookahead` through the orphan loop, with:

```cpp
  auto &longNoteLookahead = longNoteLookaheadScratch;
  longNoteLookahead.clear();
  const auto rememberLongNoteHead =
      [&](bms_parser::LongNote *longNote, float headY,
          const auto &orderProvider) {
        auto [it, inserted] = longNoteLookahead.try_emplace(longNote);
        it->second.headY = headY;
        if (!inserted) {
          return;
        }
        it->second.renderBudgetReserved =
            chartEntityRenderBudget.tryConsume(
                gameplay_chart_entity_render_budget::
                    kLongNoteReservationCost);
        if (it->second.renderBudgetReserved) {
          it->second.order = orderProvider();
        }
      };
  for (auto *orphanLongNote : state.orphanLongNotes) {
    rememberLongNoteHead(orphanLongNote, lowerBound,
                         [&]() { return pastLongNoteOrder; });
  }
```

`try_emplace` is essential: if an orphan entry is encountered again while walking rows, its `headY` may update but it is neither charged twice nor assigned another submission order.

In `keepDeadLongNoteBody`, replace the direct map assignment with:

```cpp
        rememberLongNoteHead(longNote, lowerBound, ensureLongOrder);
```

For a live long-note head, replace the direct map assignment with:

```cpp
            rememberLongNoteHead(longNote, y, ensureLongOrder);
```

For a passed long-note head, replace the direct map assignment with:

```cpp
            rememberLongNoteHead(longNote, lowerBound, ensureLongOrder);
```

At a matched tail, pass the stored authorization:

```cpp
              drawLongNote(it->second.headY, y, longNote->Head,
                           it->second.order,
                           it->second.renderBudgetReserved);
```

For the missing-lookahead fallback, make the tail row request an atomic reservation and pass it explicitly:

```cpp
              const bool renderBudgetReserved =
                  chartEntityRenderBudget.tryConsume(
                      gameplay_chart_entity_render_budget::
                          kLongNoteReservationCost);
              drawLongNote(lowerBound, y, longNote->Head, pastLongNoteOrder,
                           renderBudgetReserved);
```

For leftover long notes after timeline traversal, pass the carried authorization:

```cpp
  for (const auto &pair : longNoteLookahead) {
    drawLongNote(pair.second.headY, upperBound, pair.first,
                 pair.second.order, pair.second.renderBudgetReserved);
  }
```

Keep replay ghost calls after this loop so all chart reservations, including unfinished long notes, retain priority.

- [ ] **Step 8: Build the renderer and run the focused geometry/budget tests**

Run:

```bash
cmake --build cmake-build-debug --target \
  gameplay_chart_entity_render_budget_tests \
  gameplay_scroll_geometry_tests \
  main -j 6
./cmake-build-debug/gameplay_chart_entity_render_budget_tests
./cmake-build-debug/gameplay_scroll_geometry_tests
```

Expected: all three targets build; both focused executables exit with status 0 and no output. Compiler errors about an old four-argument `drawLongNote` call indicate a call site was missed and must be changed to pass its reservation boolean.

- [ ] **Step 9: Inspect the integration for coverage and ordering before committing**

Run:

```bash
rg -n "tryConsume|renderBudgetReserved|drawLongNote\\(" \
  src/scene/play/BMSRenderer.h src/scene/play/BMSRenderer.cpp
git diff --check
git diff -- src/scene/play/BMSRenderer.h src/scene/play/BMSRenderer.cpp
```

Expected: the diff contains one frame reset; costs for measure, normal, landmine, both invisible forms, ghost outline, and miss-marker X; all long-note calls carry authorization; no whitespace errors appear. Confirm that replay calls still follow chart traversal and that no budget failure exits the timeline loop.

- [ ] **Step 10: Commit the renderer integration**

```bash
git add src/scene/play/BMSRenderer.h src/scene/play/BMSRenderer.cpp
git commit -m "fix: cap gameplay chart entity rendering"
```

---

### Task 3: Run the Complete Regression Suite

**Files:**
- Verify only; no source changes are expected.

**Interfaces:**
- Consumes: the tested budget component and renderer integration from Tasks 1 and 2.
- Produces: build and regression evidence for the complete change.

- [ ] **Step 1: Build the desktop application from the repository's established debug tree**

Run:

```bash
cmake --build cmake-build-debug --target main -j 6
```

Expected: `main` completes successfully with no compiler or linker errors.

- [ ] **Step 2: Run the complete CTest suite**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: CTest reports `100% tests passed, 0 tests failed`.

- [ ] **Step 3: Verify the committed scope is clean and complete**

Run:

```bash
git status --short
git log -3 --oneline
```

Expected: the worktree is clean. The latest commits are `fix: cap gameplay chart entity rendering`, `feat: add gameplay chart render budget`, and the already-approved design commit `docs: design gameplay chart render budget`.

