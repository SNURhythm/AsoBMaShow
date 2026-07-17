#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace gameplay_scroll_geometry {

inline float renderY(double itemScrollPosition,
                     double currentScrollPosition, float rxhs,
                     float judgeY) {
  return judgeY +
         static_cast<float>(itemScrollPosition - currentScrollPosition) * rxhs;
}

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

inline bool shouldStopTimelineTraversal(
    bool timelineIsFuture, bool hasOpenLongNote,
    std::span<const double> suffixMinimum,
    std::span<const double> suffixMaximum, std::size_t timelineIndex,
    ScrollRange visible) {
  return timelineIsFuture && !hasOpenLongNote &&
         !suffixCanReachVisibleRange(suffixMinimum, suffixMaximum,
                                     timelineIndex, visible);
}

inline bool noteRectangleIntersectsViewport(float y, float noteHeight,
                                            float lowerBound,
                                            float upperBound) {
  return y + noteHeight >= lowerBound && y <= upperBound;
}

inline bool shouldDrawMeasureLine(long long timelineTimeMicros,
                                  long long currentTimeMicros, float y,
                                  float judgeY, float upperBound) {
  return timelineTimeMicros >= currentTimeMicros && y >= judgeY &&
         y <= upperBound;
}

} // namespace gameplay_scroll_geometry
