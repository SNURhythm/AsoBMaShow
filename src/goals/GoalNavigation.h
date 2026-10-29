#pragma once
#include <algorithm>

namespace goal_navigation {
enum class ChartTab { Ongoing, Recommendations, Achieved };
struct State {
  ChartTab chartTab = ChartTab::Ongoing;
  bool folderAchieved = false;
};
inline constexpr float columnGap = 8;
struct Columns {
  float navigationWidth = 0, bodyWidth = 0, leftPadding = 0, rightPadding = 0;
};
inline Columns columns(float width, bool navigation) {
  width = std::max(0.0f, width);
  Columns result;
  result.navigationWidth = navigation ? std::min(width * 0.4f, std::clamp(width * 0.24f, 180.0f, 280.0f)) : 0;
  const float occupied = navigation ? result.navigationWidth + columnGap : 0;
  result.bodyWidth = std::min(960.0f, std::max(0.0f, width - occupied));
  result.leftPadding = std::max(0.0f, (width - result.bodyWidth) / 2 - occupied);
  result.rightPadding = std::max(0.0f, width - result.leftPadding - occupied - result.bodyWidth);
  return result;
}
}
