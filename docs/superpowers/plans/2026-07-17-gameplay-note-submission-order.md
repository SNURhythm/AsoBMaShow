# Gameplay Note Submission Order Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace fixed note-type depths with frame-local submission-order depths, while keeping every long-note head and tail above its own body and ordering the whole long note from its head row.

**Architecture:** Add a small monotonic depth allocator shared by the textured-note and invisible-note batch paths. Stream note geometry through one reusable texture batch, flushing only when its depth or texture changes; capture two tokens at every rendered long-note head so deferred body and tail geometry sorts with the head row.

**Tech Stack:** C++23, bgfx `DepthAscending` view sorting, existing `TexBatchRenderer` and `SimpleBatchRenderer`, CMake/CTest.

## Global Constraints

- Rendering order follows chart-row traversal, not permanent note-type layers.
- Playable notes and landmines precede enabled invisible notes within the same row.
- Long-note body and tail retain the order captured at the head row.
- A long note's head and tail always render above its own body.
- Do not change note geometry, judge-line clipping, expiration, invisible-note appearance, gameplay timing, or chart parsing.
- Keep lane beams below ordered notes and ghosts/world overlays above ordered notes.
- `src/bms_parser.hpp` and `src/bms_parser.cpp` must not be edited.

## File Structure

- Create `src/scene/play/GameplayNoteSubmissionOrder.h`: frame-local order allocator and the fixed depth bands surrounding ordered notes.
- Modify `src/scene/play/GameplayScrollGeometry.h`: remove the temporary fixed `NoteRenderLayer` hierarchy.
- Modify `src/scene/play/StartLaneIndicatorGeometry.h`: source lane-cover and indicator depths from the new shared depth bands.
- Modify `src/scene/play/BMSRenderer.h`: store long-note head order, one reusable texture batch, and active streaming depths.
- Modify `src/scene/play/BMSRenderer.cpp`: allocate order during row traversal, reorder landmine/invisible phases, and submit deferred long geometry with head order.
- Modify `tests/gameplay_scroll_geometry_tests.cpp`: regression-test monotonic row order, frame reset, long-note order capture, and surrounding depth bands.
- Modify `tests/start_lane_indicator_geometry_tests.cpp`: verify lane-cover and indicator depths remain above ghosts and in their original relative order.

---

### Task 1: Submission-ordered note rendering

**Files:**
- Create: `src/scene/play/GameplayNoteSubmissionOrder.h`
- Modify: `src/scene/play/GameplayScrollGeometry.h`
- Modify: `src/scene/play/StartLaneIndicatorGeometry.h`
- Modify: `src/scene/play/BMSRenderer.h`
- Modify: `src/scene/play/BMSRenderer.cpp`
- Test: `tests/gameplay_scroll_geometry_tests.cpp`
- Test: `tests/start_lane_indicator_geometry_tests.cpp`

**Interfaces:**
- Produces: `gameplay_note_submission_order::Allocator::next()` returning the next `uint32_t` depth token.
- Produces: `gameplay_note_submission_order::Allocator::captureLongNote()` returning `LongNoteOrder{bodyDepth, endpointDepth}` with `bodyDepth < endpointDepth`.
- Produces: shared constants `kBackgroundDepth`, `kLaneBeamDepth`, `kFirstOrderedNoteDepth`, `kGhostDepth`, `kLaneCoverDepth`, `kIndicatorDepth`, `kJudgementIndicatorDepth`, and `kGaugeDepth`.
- Consumes: bgfx main view remains `bgfx::ViewMode::DepthAscending`.

- [ ] **Step 1: Write the failing allocator and depth-band tests**

Add the new include and replace the temporary fixed-type hierarchy assertions at the end of `tests/gameplay_scroll_geometry_tests.cpp`:

```cpp
#include "scene/play/GameplayNoteSubmissionOrder.h"

using gameplay_note_submission_order::Allocator;
using gameplay_note_submission_order::kFirstOrderedNoteDepth;
using gameplay_note_submission_order::kGhostDepth;
using gameplay_note_submission_order::kLaneBeamDepth;

Allocator order;
const uint32_t firstRowPrimary = order.next();
const uint32_t firstRowInvisible = order.next();
const auto longAtSecondRow = order.captureLongNote();
const uint32_t secondRowInvisible = order.next();
const uint32_t tailRowPrimary = order.next();

require(firstRowPrimary == kFirstOrderedNoteDepth,
        "the first row starts at the ordered-note depth band");
require(firstRowPrimary < firstRowInvisible,
        "an invisible phase follows its row's primary phase");
require(firstRowInvisible < longAtSecondRow.bodyDepth,
        "a later row follows every phase of an earlier row");
require(longAtSecondRow.bodyDepth < longAtSecondRow.endpointDepth,
        "long-note endpoints render above their own body");
require(longAtSecondRow.endpointDepth < secondRowInvisible,
        "same-row invisibles follow the long-note endpoints");
require(longAtSecondRow.endpointDepth < tailRowPrimary,
        "deferred long-note endpoints retain head-row order");

order.reset();
require(order.next() == kFirstOrderedNoteDepth,
        "a new frame resets submission order");
require(kLaneBeamDepth < kFirstOrderedNoteDepth,
        "lane beams stay below ordered notes");
require(tailRowPrimary < kGhostDepth,
        "ordered notes stay below ghosts");
```

Extend `tests/start_lane_indicator_geometry_tests.cpp`:

```cpp
#include "scene/play/GameplayNoteSubmissionOrder.h"

check(kLaneCoverDepth > gameplay_note_submission_order::kGhostDepth,
      "lane cover renders above ghosts and ordered notes");
check(kIndicatorDepth > kLaneCoverDepth,
      "overlapping triangle renders above the lane cover");
```

- [ ] **Step 2: Run the focused tests to verify RED**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests start_lane_indicator_geometry_tests -j 6
```

Expected: compilation fails because `GameplayNoteSubmissionOrder.h` and its allocator/constants do not exist.

- [ ] **Step 3: Add the frame-local submission-order allocator**

Create `src/scene/play/GameplayNoteSubmissionOrder.h`:

```cpp
#pragma once

#include <cassert>
#include <cstdint>

namespace gameplay_note_submission_order {

inline constexpr uint32_t kBackgroundDepth = 100U;
inline constexpr uint32_t kLaneBeamDepth = 180U;
inline constexpr uint32_t kFirstOrderedNoteDepth = 181U;
inline constexpr uint32_t kGhostDepth = 0x80000000U;
inline constexpr uint32_t kLaneCoverDepth = 0x90000000U;
inline constexpr uint32_t kIndicatorDepth = 0xA0000000U;
inline constexpr uint32_t kJudgementIndicatorDepth = 0xB0000000U;
inline constexpr uint32_t kGaugeDepth = 0xC0000000U;

struct LongNoteOrder {
  uint32_t bodyDepth = 0;
  uint32_t endpointDepth = 0;
};

class Allocator {
public:
  [[nodiscard]] uint32_t next() {
    assert(nextDepth < kGhostDepth && "note submission order exhausted");
    return nextDepth++;
  }

  [[nodiscard]] LongNoteOrder captureLongNote() {
    const uint32_t bodyDepth = next();
    return {.bodyDepth = bodyDepth, .endpointDepth = next()};
  }

  void reset() { nextDepth = kFirstOrderedNoteDepth; }

private:
  uint32_t nextDepth = kFirstOrderedNoteDepth;
};

} // namespace gameplay_note_submission_order
```

The available note band is vastly larger than bgfx's configured maximum draw-call count, so the assertion documents an unreachable frame overflow without adding fallback ordering.

- [ ] **Step 4: Replace temporary fixed note layers and preserve overlay bands**

Delete `NoteRenderLayer` and `noteRenderDepth()` from `src/scene/play/GameplayScrollGeometry.h` and remove its now-unused `<cstdint>` include if no other declaration needs it.

In `src/scene/play/StartLaneIndicatorGeometry.h`, include the new header and alias the existing public constants:

```cpp
#include "GameplayNoteSubmissionOrder.h"

inline constexpr uint32_t kLaneCoverDepth =
    gameplay_note_submission_order::kLaneCoverDepth;
inline constexpr uint32_t kIndicatorDepth =
    gameplay_note_submission_order::kIndicatorDepth;
```

In `BMSRenderer.cpp`, replace local world-depth values with the new constants:

```cpp
using gameplay_note_submission_order::kBackgroundDepth;
using gameplay_note_submission_order::kGaugeDepth;
using gameplay_note_submission_order::kGhostDepth;
using gameplay_note_submission_order::kJudgementIndicatorDepth;
using gameplay_note_submission_order::kLaneBeamDepth;
```

Use these constants for background, beams, ghosts, judgement indicator, and world gauge. Remove `kDepthInvisibleNotes`, `kDepthLongBodies`, `kDepthNotes`, and their type-hierarchy assertions.

- [ ] **Step 5: Convert the note texture path to one streaming batch**

In `BMSRenderer.h`, replace `noteTextureBatchRenderers`, `noteTextureBatchLookup`, `longBodySubmitDepth`, and `noteSheetSubmitDepth` with:

```cpp
rendering::TexBatchRenderer noteTextureBatchRenderer;
uint32_t activeNoteTextureDepth = std::numeric_limits<uint32_t>::max();
uint32_t activeInvisibleDepth = std::numeric_limits<uint32_t>::max();
```

Add `<limits>` if the header does not already include it. Replace the old batch lookup declarations with:

```cpp
rendering::TexBatchRenderer &noteTextureBatchAtDepth(uint32_t submitDepth);
void setInvisibleBatchDepth(uint32_t submitDepth);
void beginOrderedNoteBatches();
void flushOrderedNoteBatches();
```

Implement them in `BMSRenderer.cpp`:

```cpp
rendering::TexBatchRenderer &
BMSRenderer::noteTextureBatchAtDepth(uint32_t submitDepth) {
  if (activeNoteTextureDepth != submitDepth) {
    noteTextureBatchRenderer.flush();
    noteTextureBatchRenderer.setSubmitDepth(submitDepth);
    activeNoteTextureDepth = submitDepth;
  }
  return noteTextureBatchRenderer;
}

void BMSRenderer::setInvisibleBatchDepth(uint32_t submitDepth) {
  if (activeInvisibleDepth == submitDepth) {
    return;
  }
  gimmickBatchRenderer.flush();
  gimmickBatchRenderer.setSubmitDepth(submitDepth);
  activeInvisibleDepth = submitDepth;
}

void BMSRenderer::beginOrderedNoteBatches() {
  activeNoteTextureDepth = std::numeric_limits<uint32_t>::max();
  activeInvisibleDepth = std::numeric_limits<uint32_t>::max();
  noteTextureBatchRenderer.begin();
  gimmickBatchRenderer.begin();
}

void BMSRenderer::flushOrderedNoteBatches() {
  noteTextureBatchRenderer.flush();
  gimmickBatchRenderer.flush();
}
```

Remove `noteTextureBatchKey`, `noteTextureBatch`, `sheetBatchFor`, `longBodyBatchFor`, `beginNoteTextureBatches`, `flushNoteTextureBatches`, and the constructor reserves for their lookup containers.

- [ ] **Step 6: Pass submission order into every note draw path**

Include `GameplayNoteSubmissionOrder.h` from `BMSRenderer.h`. Change draw declarations and definitions to:

```cpp
void drawLongNote(
    float headY, float tailY, bms_parser::LongNote *const &head,
    gameplay_note_submission_order::LongNoteOrder order);
void drawNormalNote(float y, bms_parser::Note *const &note,
                    uint32_t submitDepth);
void drawInvisibleNote(float y, bms_parser::Note *const &note,
                       uint32_t submitDepth);
void drawLandmineNote(float y, bms_parser::LandmineNote *const &note,
                      uint32_t submitDepth);
```

Before adding visible invisible-note rectangles, call:

```cpp
setInvisibleBatchDepth(submitDepth);
```

Replace normal-note and landmine `sheetBatchFor(sheet)` calls with:

```cpp
noteTextureBatchAtDepth(submitDepth)
```

Within `drawLongNote`, submit the body through:

```cpp
auto &bodyBatch = noteTextureBatchAtDepth(order.bodyDepth);
```

Submit both head and tail through:

```cpp
auto &endpointBatch = noteTextureBatchAtDepth(order.endpointDepth);
```

This separate body/endpoint token is required even when textures differ, because bgfx sorts equal-depth calls by shader program and does not promise call-order layering.

- [ ] **Step 7: Retain head order in long-note lookahead**

In `BMSRenderer.h`, replace the float lookahead value with:

```cpp
struct LongNoteLookahead {
  float headY = 0.0F;
  gameplay_note_submission_order::LongNoteOrder order;
};

std::unordered_map<bms_parser::LongNote *, LongNoteLookahead>
    longNoteLookaheadScratch;
```

At the start of `render`, create a frame allocator and reserve the earliest order for long notes whose heads precede the current render window:

```cpp
gameplay_note_submission_order::Allocator submissionOrder;
const auto pastLongNoteOrder = submissionOrder.captureLongNote();

for (auto *orphanLongNote : state.orphanLongNotes) {
  longNoteLookahead[orphanLongNote] = {
      .headY = lowerBound,
      .order = pastLongNoteOrder,
  };
}
```

For each traversed row, detect whether it contains a long-note head and lazily capture its primary order:

```cpp
const bool rowHasLongHead =
    i < groupedTimelineNotes.size() &&
    std::any_of(groupedTimelineNotes[i].begin(),
                groupedTimelineNotes[i].end(), [](const auto *note) {
                  if (note == nullptr || !note->IsLongNote()) {
                    return false;
                  }
                  return !static_cast<const bms_parser::LongNote *>(note)
                              ->IsTail();
                });

std::optional<gameplay_note_submission_order::LongNoteOrder> rowLongOrder;
std::optional<uint32_t> rowPrimaryDepth;
const auto ensurePrimaryDepth = [&]() {
  if (!rowPrimaryDepth.has_value()) {
    if (rowHasLongHead) {
      rowLongOrder = submissionOrder.captureLongNote();
      rowPrimaryDepth = rowLongOrder->endpointDepth;
    } else {
      rowPrimaryDepth = submissionOrder.next();
    }
  }
  return *rowPrimaryDepth;
};
const auto ensureLongOrder = [&]() {
  (void)ensurePrimaryDepth();
  assert(rowLongOrder.has_value());
  return *rowLongOrder;
};
```

Store a head with both geometry and order:

```cpp
longNoteLookahead[longNote] = {
    .headY = y,
    .order = ensureLongOrder(),
};
```

Replace the two existing paths that keep a passed/dead long-note head at
`lowerBound` with the same complete value:

```cpp
longNoteLookahead[longNote] = {
    .headY = lowerBound,
    .order = ensureLongOrder(),
};
```

At a tail, render with the stored values:

```cpp
drawLongNote(it->second.headY, y, longNote->Head, it->second.order);
```

Use `pastLongNoteOrder` for the existing missing-lookahead fallback and for already-active orphan heads. Render leftover long notes with `pair.second.headY` and `pair.second.order`.

- [ ] **Step 8: Submit each row in Beatoraja order**

Pass `ensurePrimaryDepth()` to normal-note and landmine draw calls. Keep long tails from allocating a tail-row primary depth; they render only with their stored head order.

Move the loop over `timeLine->LandmineNotes` before the loop over `timeLine->InvisibleNotes`. Allocate one invisible token lazily after every playable/landmine phase:

```cpp
std::optional<uint32_t> rowInvisibleDepth;
for (const auto &note : timeLine->InvisibleNotes) {
  if (note == nullptr || note->IsDead) {
    continue;
  }
  if (timeLine->Timing >= chartTimeMicros) {
    if (showInvisibleNotes) {
      if (!rowInvisibleDepth.has_value()) {
        rowInvisibleDepth = submissionOrder.next();
      }
      drawInvisibleNote(y, note, *rowInvisibleDepth);
    }
  } else {
    note->IsDead = true;
  }
}
```

At frame setup, call `beginOrderedNoteBatches()` after beginning the background and ghost batches. At the note flush point, call `flushOrderedNoteBatches()` instead of the old texture/gimmick flush functions.

- [ ] **Step 9: Run focused tests and desktop compilation to verify GREEN**

Run:

```bash
cmake --build cmake-build-debug --target gameplay_scroll_geometry_tests start_lane_indicator_geometry_tests main -j 6
./cmake-build-debug/gameplay_scroll_geometry_tests
./cmake-build-debug/start_lane_indicator_geometry_tests
```

Expected: both test executables exit 0 and `main` links successfully. Existing bgfx header warnings are acceptable; new warnings are not.

- [ ] **Step 10: Commit the implementation**

```bash
git add \
  src/scene/play/GameplayNoteSubmissionOrder.h \
  src/scene/play/GameplayScrollGeometry.h \
  src/scene/play/StartLaneIndicatorGeometry.h \
  src/scene/play/BMSRenderer.h \
  src/scene/play/BMSRenderer.cpp \
  tests/gameplay_scroll_geometry_tests.cpp \
  tests/start_lane_indicator_geometry_tests.cpp
git commit -m "fix: order note rendering by chart row"
```

---

### Task 2: Full regression verification

**Files:**
- Verify only; no new files.

**Interfaces:**
- Consumes: the row-ordered renderer from Task 1.
- Produces: fresh build and test evidence for handoff.

- [ ] **Step 1: Verify formatting and the full desktop build**

Run:

```bash
git diff --check HEAD~1..HEAD
cmake --build cmake-build-debug --target main -j 6
```

Expected: `git diff --check` prints nothing and the `main` target exits 0.

- [ ] **Step 2: Run the complete test suite**

Run:

```bash
ctest --test-dir cmake-build-debug --output-on-failure
```

Expected: all configured tests pass with zero failures.

- [ ] **Step 3: Confirm repository state and implementation commit**

Run:

```bash
git status --short
git log -2 --oneline
```

Expected: the working tree is clean and the latest implementation commit is `fix: order note rendering by chart row`.
