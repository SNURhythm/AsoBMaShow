#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace start_lane_indicator {

enum class ColorRole { White, Blue, Red };

inline constexpr float kWidthLaneRatio = 0.46F;
inline constexpr float kHeightLaneRatio = 0.40F;
inline constexpr float kCoverGap = 0.08F;
inline constexpr uint32_t kLaneCoverDepth = 300;
inline constexpr uint32_t kIndicatorDepth = 320;

struct Triangle {
  float leftX = 0.0F;
  float rightX = 0.0F;
  float baseY = 0.0F;
  float tipX = 0.0F;
  float tipY = 0.0F;
  bool overlapsCover = false;
};

inline ColorRole colorRoleForKey(std::size_t position,
                                 std::size_t keyCount) {
  if (keyCount == 0 || position >= keyCount) {
    return ColorRole::White;
  }
  const std::size_t mirrored =
      std::min(position, keyCount - position - 1);
  return (mirrored & 1U) == 0 ? ColorRole::White : ColorRole::Blue;
}

inline ColorRole colorRoleForScratch() { return ColorRole::Red; }

inline Triangle placeTriangle(float laneLeftX, float laneWidth, float judgeY,
                              float coverEdgeY) {
  const float width = laneWidth * kWidthLaneRatio;
  const float height = laneWidth * kHeightLaneRatio;
  const float left = laneLeftX + (laneWidth - width) * 0.5F;
  const float desiredBase = coverEdgeY - kCoverGap;
  const float baseY = std::max(desiredBase, judgeY + height);
  return {.leftX = left,
          .rightX = left + width,
          .baseY = baseY,
          .tipX = laneLeftX + laneWidth * 0.5F,
          .tipY = baseY - height,
          .overlapsCover = baseY >= coverEdgeY};
}

} // namespace start_lane_indicator
