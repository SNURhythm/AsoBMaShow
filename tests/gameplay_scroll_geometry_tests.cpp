#include "scene/play/GameplayScrollGeometry.h"
#include "scene/play/GameplayNoteSubmissionOrder.h"

#include <cmath>
#include <cstddef>
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

void requireNear(double actual, double expected, const char *message) {
  require(std::fabs(actual - expected) < 0.0000001, message);
}
} // namespace

int main() {
  using namespace gameplay_scroll_geometry;

  require(chartRenderTimeMicros(3'750'075) == 3'750'075,
          "chart traversal preserves sub-millisecond visual time");
  require(chartRenderTimeMicros(3'750'999) == 3'750'999,
          "chart traversal does not collapse one millisecond of content");
  require(chartRenderTimeMicros(-1'999) == -1'999,
          "negative preroll preserves sub-millisecond visual time");
  require(chartRenderTimeMicros(4'000'000) == 4'000'000,
          "an exact millisecond is unchanged");

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

  require(shouldDrawMeasureLine(1'000'000, 1'000'000, 0.5F, 0.5F, 8.5F),
          "a measure line at the current timing remains visible");
  require(!shouldDrawMeasureLine(999'999, 1'000'000, -0.5F, 0.5F, 8.5F),
          "a passed measure line is hidden");
  require(!shouldDrawMeasureLine(1'100'000, 1'000'000, -0.5F, 0.5F, 8.5F),
          "a future measure line below the judge line is hidden");
  require(!shouldDrawMeasureLine(1'100'000, 1'000'000, 9.0F, 0.5F, 8.5F),
          "a future measure line above the visible lane is hidden");

  require(!shouldKeepRenderTimeline(145.0, 145.0, 0.0, 1.0, 1.0, 1.0, 1.0,
                                    false, false, false, false, false),
          "a BGA-only row is omitted from render traversal");
  require(shouldKeepRenderTimeline(145.0, 290.0, 0.0, 1.0, 1.0, 1.0, 1.0,
                                   false, false, false, false, false),
          "a BPM change remains in render traversal");
  require(shouldKeepRenderTimeline(145.0, 145.0, 0.0, 1.0, -10'000.0, 1.0,
                                   1.0, false, false, false, false, false),
          "a scroll change remains in render traversal");
  require(shouldKeepRenderTimeline(145.0, 145.0, 0.0, 1.0, 1.0, 1.0, 1.0,
                                   false, false, true, false, false),
          "a playable note remains in render traversal");
  require(shouldKeepRenderTimeline(145.0, 145.0, 0.0, 1.0, 1.0, 1.0, 1.0,
                                   false, false, false, true, false),
          "an invisible note remains in render traversal");
  require(shouldKeepRenderTimeline(145.0, 145.0, 0.0, 1.0, 1.0, 1.0, 1.0,
                                   false, false, false, false, true),
          "a landmine remains in render traversal");
  require(shouldKeepRenderTimeline(145.0, 145.0, 0.0, 1.0, 1.0, 1.0, 1.0,
                                   true, false, false, false, false),
          "an authored SPEED object remains in render traversal");

  const std::array<ScrollPositionTimeline, 2> positiveLeadIn{{
      {.timeMicros = 1'000'000,
       .scrollPosition = 4.0,
       .bpm = 120.0,
       .scrollRate = 1.0},
      {.timeMicros = 2'000'000,
       .scrollPosition = 8.0,
       .bpm = 120.0,
       .scrollRate = 1.0},
  }};
  requireNear(scrollPositionAtTime(positiveLeadIn, -1), 0.0,
              "pre-first positive-time traversal clamps to the origin");
  requireNear(
      scrollPositionAtTime(positiveLeadIn, 500'000), 2.0,
      "pre-first positive-time traversal interpolates to the first row");
  requireNear(
      scrollPositionAtTime(positiveLeadIn, 1'000'000), 4.0,
      "an exact first positive timestamp selects its authored position");

  const std::array<ScrollPositionTimeline, 2> negativeLeadIn{{
      {.timeMicros = -1'000'000,
       .scrollPosition = -4.0,
       .bpm = 120.0,
       .scrollRate = -2.0},
      {.timeMicros = 0,
       .scrollPosition = -2.0,
       .bpm = 120.0,
       .scrollRate = -2.0},
  }};
  requireNear(scrollPositionAtTime(negativeLeadIn, -2'000'000), -3.0,
              "pre-first negative-time traversal uses signed lead-in scroll");
  requireNear(
      scrollPositionAtTime(negativeLeadIn, -1'000'000), -4.0,
      "an exact negative first timestamp selects its authored position");

  const std::array<ScrollPositionTimeline, 4> stopAndEqualRows{{
      {.timeMicros = 0,
       .scrollPosition = 0.0,
       .bpm = 120.0,
       .scrollRate = -1.0},
      {.timeMicros = 1'000'000,
       .scrollPosition = -4.0,
       .stopMicros = 200'000,
       .bpm = 120.0,
       .scrollRate = -1.0},
      {.timeMicros = 1'000'000,
       .scrollPosition = -5.0,
       .bpm = 120.0,
       .scrollRate = -1.0},
      {.timeMicros = 2'000'000,
       .scrollPosition = -9.0,
       .bpm = 120.0,
       .scrollRate = -1.0},
  }};
  requireNear(scrollPositionAtTime(stopAndEqualRows, 900'000), -3.6,
              "reverse travel interpolates before a stop");
  requireNear(
      scrollPositionAtTime(stopAndEqualRows, 1'000'000), -4.0,
      "equal timestamps select the first retained row like BMSRenderer");
  requireNear(scrollPositionAtTime(stopAndEqualRows, 1'100'000), -5.4,
              "the later equal row owns subsequent reverse travel");
  requireNear(scrollPositionAtTime(stopAndEqualRows, 2'000'000), -9.0,
              "an exact post-stop boundary selects the next authored row");

  requireNear(initialFutureTimelineY(0.0, -0.5, 2.0F, 0.5F), 1.5F,
              "a zero-time first row moves during negative preroll");
  requireNear(advanceFutureTimelineY(0.5, 1.0, 1.0, 0, 0.0, 1'000, 500, 10.0),
              5.5, "future Y advances by remaining segment travel");
  requireNear(
      advanceFutureTimelineY(0.5, 1.0, 1.0, 0, 1'000.0, 2'000, 500, 10.0), 10.5,
      "an active stop uses the full section distance");

  const double collapsedY = advanceFutureTimelineY(0.5, 1.0, 20'000.0, 1'000,
                                                   0.0, 1'000, 1'000, 10.0);
  require(std::isnan(collapsedY),
          "a huge-BPM zero-duration pair forms a traversal boundary");
  require(futureTimelineTraversalContinues(10.0, 10.0F),
          "a row exactly at the lane top is processed");
  require(!futureTimelineTraversalContinues(10.1, 10.0F),
          "the first row above the lane top ends traversal");
  require(!futureTimelineTraversalContinues(collapsedY, 10.0F),
          "a non-finite collapsed row ends traversal");
  const double reverseFutureY =
      advanceFutureTimelineY(0.5, 1.0, -20'000.0, 1'000, 0.0, 1'000, 500, 10.0);
  requireNear(reverseFutureY, -199'999.5,
              "a future reverse row uses its full signed section distance");
  require(futureTimelineTraversalContinues(reverseFutureY, 10.0F),
          "a reverse row below the judge line keeps traversal active");

  const ScrollRange visible =
      visibleScrollRange(0.0, 1.0F, -1.0F, 10.0F, 1.0F, 0.0F);
  requireNear(visible.minimum, -2.0,
              "visible scroll range includes a partially visible note below");
  requireNear(visible.maximum, 10.0,
              "visible scroll range ends at the upper viewport bound");

  require(noteRectangleIntersectsViewport(-1.5F, 1.0F, -1.0F, 10.0F),
          "a note crossing the lower viewport bound remains visible");
  require(!noteRectangleIntersectsViewport(-2.1F, 1.0F, -1.0F, 10.0F),
          "a note entirely below the viewport is culled");
  require(!noteRectangleIntersectsViewport(10.1F, 1.0F, -1.0F, 10.0F),
          "a note entirely above the viewport is culled");
  require(noteRectangleIntersectsViewport(-0.5F, 1.0F, -1.0F, 10.0F),
          "crossing the judge line does not hide a normal note");

  require(!shouldDrawNoteRectangle(1'100'000, 1'000'000, -1.1F, 1.0F, 0.0F),
          "a future note entirely below the judge line is hidden");
  require(shouldDrawNoteRectangle(1'100'000, 1'000'000, -0.5F, 1.0F, 0.0F),
          "a future note crossing the judge line remains visible");
  require(!shouldDrawNoteRectangle(1'000'000, 1'000'000, -1.1F, 1.0F, 0.0F),
          "a current note entirely below the judge line is hidden");
  require(shouldDrawNoteRectangle(999'999, 1'000'000, -1.1F, 1.0F, 0.0F),
          "a past note below the judge line remains visible");

  const auto clippedFuture =
      clipNoteRectangle(1'100'000, 1'000'000, -0.5F, 1.0F, 0.0F);
  require(clippedFuture.visible,
          "a future note crossing the judge line remains drawable");
  requireNear(clippedFuture.y, 0.0F,
              "future note geometry starts at the judge line");
  requireNear(clippedFuture.height, 0.5F,
              "future note geometry keeps only the visible height");
  requireNear(clippedFuture.bottomTextureFraction, 0.5F,
              "future note texture is cropped at the same ratio");

  const auto untouchedPast =
      clipNoteRectangle(999'999, 1'000'000, -0.5F, 1.0F, 0.0F);
  require(untouchedPast.visible, "a past note remains drawable");
  requireNear(untouchedPast.y, -0.5F,
              "past note geometry stays below the judge line");
  requireNear(untouchedPast.height, 1.0F, "past note keeps its full height");
  requireNear(untouchedPast.bottomTextureFraction, 1.0F,
              "past note keeps its full texture");

  const auto fullOutline = noteOutlineRectangles(
      2.0F, -0.5F, 1.0F, 1.0F, kInvisibleNoteBorderHeightRatio, untouchedPast);
  require(fullOutline.count == 4,
          "an unclipped invisible note has four border segments");
  requireNear(fullOutline.rectangles[0].height, 0.15F,
              "the gameplay invisible-note border uses the thicker ratio");
  bool centerCovered = false;
  for (std::size_t i = 0; i < fullOutline.count; ++i) {
    const auto &rect = fullOutline.rectangles[i];
    centerCovered =
        centerCovered || (2.5F > rect.x && 2.5F < rect.x + rect.width &&
                          0.0F > rect.y && 0.0F < rect.y + rect.height);
  }
  require(!centerCovered, "the invisible-note outline leaves its center empty");

  const auto clippedOutline = noteOutlineRectangles(
      2.0F, -0.5F, 1.0F, 1.0F, kInvisibleNoteBorderHeightRatio, clippedFuture);
  require(clippedOutline.count == 3,
          "judge-line clipping removes the hidden bottom border");
  requireNear(clippedOutline.rectangles[0].y, 0.35F,
              "the visible top border keeps its original position");
  requireNear(clippedOutline.rectangles[1].y, 0.0F,
              "the left border starts at the clip boundary");
  requireNear(clippedOutline.rectangles[1].height, 0.5F,
              "the side border is clipped to the visible note height");

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
  require(tailRowPrimary < kGhostDepth, "ordered notes stay below ghosts");
  return 0;
}
