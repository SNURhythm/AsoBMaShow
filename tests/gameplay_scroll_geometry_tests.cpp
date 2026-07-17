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

  requireNear(initialFutureTimelineY(0.0, -0.5, 2.0F, 0.5F), 1.5F,
              "a zero-time first row moves during negative preroll");
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
  const double reverseFutureY = advanceFutureTimelineY(
      0.5, 1.0, -20'000.0, 1'000, 0.0, 1'000, 500, 10.0);
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

  require(!shouldDrawNoteRectangle(1'100'000, 1'000'000, -1.1F, 1.0F,
                                   0.0F),
          "a future note entirely below the judge line is hidden");
  require(shouldDrawNoteRectangle(1'100'000, 1'000'000, -0.5F, 1.0F,
                                  0.0F),
          "a future note crossing the judge line remains visible");
  require(!shouldDrawNoteRectangle(1'000'000, 1'000'000, -1.1F, 1.0F,
                                   0.0F),
          "a current note entirely below the judge line is hidden");
  require(shouldDrawNoteRectangle(999'999, 1'000'000, -1.1F, 1.0F,
                                  0.0F),
          "a past note below the judge line remains visible");
  return 0;
}
