#include "scene/play/GameplayScrollGeometry.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

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
  require(!shouldStopTimelineTraversal(false, false, reentrySuffix.minimum,
                                       reentrySuffix.maximum, 4, visible),
          "a past timeline cannot stop lifecycle traversal");
  require(shouldStopTimelineTraversal(true, false, reentrySuffix.minimum,
                                      reentrySuffix.maximum, 4, visible),
          "an unreachable future suffix stops traversal");
  require(!shouldStopTimelineTraversal(true, false, reentrySuffix.minimum,
                                       reentrySuffix.maximum, 1, visible),
          "a reachable future suffix keeps traversal alive");
  require(!shouldStopTimelineTraversal(true, true, reentrySuffix.minimum,
                                       reentrySuffix.maximum, 4, visible),
          "an open long note keeps traversal alive until its tail");

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
  return 0;
}
