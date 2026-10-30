#pragma once
#include <algorithm>
#include <map>
#include <optional>
#include <string>

namespace goal_navigation {
enum class ChartTab { Ongoing, Recommendations, Achieved };
struct State {
  ChartTab chartTab = ChartTab::Ongoing;
  bool folderAchieved = false;
  int chartOffset = 0, chartAchievedOffset = 0;
  int folderOffset = 0, folderAchievedOffset = 0;
  std::map<std::string, float> chartScrollOffsets, folderScrollOffsets;
  float chartNavigationOffset = 0, folderNavigationOffset = 0;
  std::optional<int> chartRecommendationRank;
  std::map<int, int> chartRecommendationOffsets;
  int chartRecommendationTable = 0;
  bool chartRecommendationSortTable = false;
};
inline constexpr float columnGap = 8;
struct Columns {
  float navigationWidth = 0, bodyWidth = 0, leftPadding = 0, rightPadding = 0;
};
inline bool stackedNavigation(float availableWidth, bool compact) {
  return compact || availableWidth < 900;
}
inline Columns columns(float width, bool navigation, bool stacked = false) {
  width = std::max(0.0f, width);
  navigation = navigation && !stacked;
  Columns result;
  result.navigationWidth = navigation ? std::min(width * 0.4f, std::clamp(width * 0.24f, 180.0f, 280.0f)) : 0;
  const float occupied = navigation ? result.navigationWidth + columnGap : 0;
  result.bodyWidth = std::min(960.0f, std::max(0.0f, width - occupied));
  result.leftPadding = std::max(0.0f, (width - result.bodyWidth) / 2 - occupied);
  result.rightPadding = std::max(0.0f, width - result.leftPadding - occupied - result.bodyWidth);
  return result;
}
}
