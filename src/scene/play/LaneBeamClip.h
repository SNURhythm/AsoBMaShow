#pragma once

#include "../../LaneCover.h"
#include <cstdint>
#include <limits>
#include <optional>

namespace lane_beam {
struct Scissor {
  uint16_t x, y, width, height;
};

// Keep the original beam vertices/gradient. Only pixels safely behind opaque
// covers are excluded, with a drawable-pixel guard for raster/MSAA boundaries.
template <typename CameraType>
std::optional<Scissor> coverScissor(
    const CameraType &camera, float left, float width, float judgeY,
    float upperBound, float visibleUpperBound, const lane_cover::State &state) {
  const auto covers = lane_cover::geometry(state, 0.0F, upperBound);
  const bool topCovered = upperBound - visibleUpperBound > 0.001F;
  const bool bottomCovered = state.hiddenEnabled && covers.hiddenY > judgeY;
  if ((!topCovered && !bottomCovered) ||
      camera.getViewWidth() == 0 || camera.getViewHeight() == 0) return std::nullopt;

  const int viewBottom = int(camera.getViewY()) + camera.getViewHeight();
  if (viewBottom > UINT16_MAX ||
      int(camera.getViewX()) + camera.getViewWidth() > UINT16_MAX) return std::nullopt;
  const float low = bottomCovered ? covers.hiddenY : judgeY;
  const float high = topCovered ? visibleUpperBound : upperBound;
  float minY = std::numeric_limits<float>::infinity();
  float maxY = -std::numeric_limits<float>::infinity();
  // All four corners retain conservative bounds even for a tilted camera.
  // Crossed cover edges intentionally retain their overlap and boundary guard.
  for (float x : {left, left + width}) {
    for (float y : {low, high}) {
      const auto point = camera.project({x, y, 0.0F});
      if (!std::isfinite(point.y) || !std::isfinite(point.z) ||
          point.z < 0.0F || point.z > 1.0F) return std::nullopt;
      minY = std::min(minY, point.y);
      maxY = std::max(maxY, point.y);
    }
  }
  const auto top = static_cast<uint16_t>(std::clamp(
      std::floor(minY) - 1.0F, float(camera.getViewY()), float(viewBottom)));
  const auto bottom = static_cast<uint16_t>(std::clamp(
      std::ceil(maxY) + 1.0F, float(camera.getViewY()), float(viewBottom)));
  if (bottom <= top || bottom - top == camera.getViewHeight()) return std::nullopt;
  return Scissor{camera.getViewX(), top, camera.getViewWidth(),
                 static_cast<uint16_t>(bottom - top)};
}
} // namespace lane_beam
