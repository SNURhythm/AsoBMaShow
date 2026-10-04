#pragma once

#include "../settings/PresentationGeometryPolicy.h"
#include <algorithm>
#include <cmath>

namespace rendering {
struct NormalizedSafeArea {
  float top = 0, right = 0, bottom = 0, left = 0;
};
struct PortraitPlayfieldFrame {
  float cameraDepth, lookAtY;
};
inline constexpr float kPlayfieldVerticalFovDegrees = 120.0F;

inline PortraitPlayfieldFrame framePortraitPlayfield(
    float laneLength, float playAreaWidth, float angleDegrees, float aspect,
    NormalizedSafeArea safeArea = {}) {
  const auto policy = player_settings::presentationGeometryPolicy(
      player_settings::PresentationOrientation::Portrait);
  const auto clamp = [](float value, player_settings::FloatSettingRange range) {
    return std::isfinite(value) ? std::clamp(value, range.minimum, range.maximum)
                                : range.defaultValue;
  };
  laneLength = clamp(laneLength, policy.length);
  playAreaWidth = clamp(playAreaWidth, policy.width);
  angleDegrees = clamp(angleDegrees, policy.angle);
  aspect = std::isfinite(aspect) && aspect > 0 ? aspect : 9.0F / 16.0F;
  const auto inset = [](float value) {
    return std::isfinite(value) ? std::clamp(value, 0.0F, 0.35F) : 0.0F;
  };
  constexpr double pi = 3.141592653589793;
  const double angle = angleDegrees * pi / 180;
  const double cosine = std::cos(angle), sine = std::sin(angle);
  const double tangent = std::tan(kPlayfieldVerticalFovDegrees * pi / 360);
  const double top = 1 - 2 * (inset(safeArea.top) + .035);
  const double bottom = -1 + 2 * (inset(safeArea.bottom) + .065);
  const double horizontal = 1 - 2 * (std::max(inset(safeArea.left), inset(safeArea.right)) + .035);
  // For a given depth d, intersect the top/bottom viewport rays with z=0:
  // y = lookAtY + d*k. Center the full lane interval [-1, length] between
  // those intersections; widen d when the nearest corners need more room.
  const auto rayCoefficient = [&](double ndc) {
    return ndc * tangent / (cosine * (cosine - ndc * tangent * sine));
  };
  const double kt = rayCoefficient(top), kb = rayCoefficient(bottom);
  const double halfHeight = (laneLength + 1) / 2.0;
  const double center = (laneLength - 1) / 2.0;
  const double average = (kt + kb) / 2;
  const double verticalDepth = 2 * halfHeight / (kt - kb);
  const double requiredNearDepth = playAreaWidth / (2 * tangent * aspect * horizontal);
  const double horizontalDepth = (requiredNearDepth + halfHeight * sine) /
                                (1 / cosine + average * sine);
  const double depth = std::max({verticalDepth, horizontalDepth, .2});
  return {static_cast<float>(depth), static_cast<float>(center - depth * average)};
}
} // namespace rendering
