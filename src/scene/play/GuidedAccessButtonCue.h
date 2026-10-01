#pragma once
#include "../../platform/IPadHardwareButton.h"
#include <algorithm>
#include <cmath>

namespace gameplay {
struct ButtonCueRect { int x = 0, y = 0, width = 0, height = 0; };
struct ButtonCueInsets { int top = 0, right = 0, bottom = 0, left = 0; };
struct ButtonCueLayout {
  ButtonCueRect marker;
  ButtonCueRect label;
};
inline ButtonCueLayout layoutButtonCue(ipad_hardware::ButtonLocation location,
                                       int width, int height, ButtonCueInsets insets = {}) {
  using ipad_hardware::Edge;
  ButtonCueLayout layout;
  // Existing centered help is the text-only fallback. An extra floating label
  // can overlap the title in a short/wide window.
  if (width <= 0 || height <= 0 || location.edge == Edge::Unknown) return layout;
  const int left = std::clamp(insets.left + 24, 0, width - 1);
  const int right = std::max(left + 1, width - insets.right - 24);
  const int top = std::clamp(insets.top + 24, 0, height - 1);
  const int bottom = std::max(top + 1, height - insets.bottom - 24);
  auto &label = layout.label;
  label.width = std::min(360, right - left);
  label.height = std::min(112, bottom - top);
  const int anchorX = std::lround(location.x * width);
  const int anchorY = std::lround(location.y * height);
  label.x = std::clamp(anchorX - label.width / 2, left, right - label.width);
  label.y = std::clamp(anchorY - label.height / 2, top, bottom - label.height);
  auto &marker = layout.marker;
  if (location.edge == Edge::Top || location.edge == Edge::Bottom) {
    marker.width = std::min(54, width);
    marker.height = std::min(6, height);
    marker.x = std::clamp(anchorX - marker.width / 2, 0, width - marker.width);
    marker.y = location.edge == Edge::Top ? std::min(6, height - marker.height)
                                        : std::max(0, height - marker.height - 6);
    label.y = location.edge == Edge::Top ? top : bottom - label.height;
  } else if (location.edge == Edge::Left || location.edge == Edge::Right) {
    marker.width = std::min(6, width);
    marker.height = std::min(54, height);
    marker.x = location.edge == Edge::Left ? std::min(6, width - marker.width)
                                         : std::max(0, width - marker.width - 6);
    marker.y = std::clamp(anchorY - marker.height / 2, 0, height - marker.height);
    label.x = location.edge == Edge::Left ? left : right - label.width;
  }
  return layout;
}
} // namespace gameplay
