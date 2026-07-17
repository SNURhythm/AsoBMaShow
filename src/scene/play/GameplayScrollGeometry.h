#pragma once

#include <algorithm>
#include <limits>

namespace gameplay_scroll_geometry {

inline long long chartRenderTimeMicros(long long visualTimeMicros) {
  return visualTimeMicros;
}

inline float renderY(double itemScrollPosition,
                     double currentScrollPosition, float rxhs,
                     float judgeY) {
  return judgeY +
         static_cast<float>(itemScrollPosition - currentScrollPosition) * rxhs;
}

inline bool shouldKeepRenderTimeline(
    double previousBpm, double bpm, double stopDurationMicros,
    double previousScroll, double scroll, bool isMeasureLine,
    bool hasPlayableNote, bool hasInvisibleNote, bool hasLandmine) {
  return previousBpm != bpm || stopDurationMicros > 0.0 ||
         previousScroll != scroll || isMeasureLine || hasPlayableNote ||
         hasInvisibleNote || hasLandmine;
}

inline float initialFutureTimelineY(double timelineScrollPosition,
                                    double currentScrollPosition, float rxhs,
                                    float judgeY) {
  return renderY(timelineScrollPosition, currentScrollPosition, rxhs, judgeY);
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

struct ScrollRange {
  double minimum = 0.0;
  double maximum = 0.0;
};

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
