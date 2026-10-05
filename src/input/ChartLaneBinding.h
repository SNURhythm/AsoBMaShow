#pragma once

#include "../bms_parser.hpp"

#include <optional>

namespace input_profile {

// Binding scopes distinguish scratchless modes; chart and replay lanes do not.
[[nodiscard]] constexpr int canonicalChartKeyMode(int keyMode) {
  return keyMode == -5 || keyMode == -7 ? -keyMode : keyMode;
}

[[nodiscard]] inline std::optional<int> chartLaneForKeyPosition(int keyMode,
                                                                int position) {
  if (position < 0) {
    return std::nullopt;
  }
  bms_parser::ChartMeta meta;
  meta.KeyMode = canonicalChartKeyMode(keyMode);
  const auto lanes = meta.GetKeyLaneIndices();
  if (static_cast<std::size_t>(position) >= lanes.size()) {
    return std::nullopt;
  }
  return lanes[static_cast<std::size_t>(position)];
}

} // namespace input_profile
