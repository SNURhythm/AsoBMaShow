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
struct ButtonCueConfirmation {
  ButtonCueLayout layout;
  float opacity = 1.0F;
};
inline ButtonCueConfirmation confirmButtonCue(ButtonCueLayout layout,
    ipad_hardware::Edge edge, int width, int height, float progress) {
  using ipad_hardware::Edge;
  if (edge == Edge::Unknown || width <= 0 || height <= 0) return {{}, 0};
  const float t = std::clamp(progress, 0.0F, 0.8F);
  const float fade = std::clamp((t - 0.30F) / 0.50F, 0.0F, 1.0F);
  const float opacity = 1.0F - fade * fade * (3.0F - 2.0F * fade);
  const float spring = std::exp(-10.0F * t) * std::sin(18.0F * t);
  const float travel = std::clamp((t - 0.18F) / 0.62F, 0.0F, 1.0F);
  const float drift = 24.0F * (1.0F - std::pow(1.0F - travel, 3.0F));
  const int dx = edge == Edge::Left ? 1 : edge == Edge::Right ? -1 : 0;
  const int dy = edge == Edge::Top ? 1 : edge == Edge::Bottom ? -1 : 0;
  const auto animateRect = [&](ButtonCueRect rect, float scaleX, float scaleY,
                                float inward) {
    const int w = std::clamp(static_cast<int>(std::lround(rect.width * scaleX)), 0, width);
    const int h = std::clamp(static_cast<int>(std::lround(rect.height * scaleY)), 0, height);
    return ButtonCueRect{
        std::clamp(rect.x + (rect.width - w) / 2 + static_cast<int>(std::lround(dx * inward)),
                   0, width - w),
        std::clamp(rect.y + (rect.height - h) / 2 + static_cast<int>(std::lround(dy * inward)),
                   0, height - h), w, h};
  };
  const bool horizontal = edge == Edge::Top || edge == Edge::Bottom;
  const float stretch = 1.0F + 1.4F * spring;
  const float thickness = 1.0F + 0.45F * spring;
  layout.marker = animateRect(layout.marker, horizontal ? stretch : thickness,
                             horizontal ? thickness : stretch, drift);
  // Pop the success capsule inward, rebound, then drift away with the marker.
  layout.label = animateRect(layout.label, 1.0F + 0.10F * spring,
                            1.0F + 0.10F * spring, drift + 18.0F * spring);
  return {layout, opacity};
}
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
