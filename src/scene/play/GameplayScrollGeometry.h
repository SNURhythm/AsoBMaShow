#pragma once

namespace gameplay_scroll_geometry {

inline float renderY(double itemScrollPosition,
                     double currentScrollPosition, float rxhs,
                     float judgeY) {
  return judgeY +
         static_cast<float>(itemScrollPosition - currentScrollPosition) * rxhs;
}

inline bool shouldDrawMeasureLine(long long timelineTimeMicros,
                                  long long currentTimeMicros, float y,
                                  float judgeY, float upperBound) {
  return timelineTimeMicros >= currentTimeMicros && y >= judgeY &&
         y <= upperBound;
}

} // namespace gameplay_scroll_geometry
