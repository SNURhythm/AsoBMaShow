#pragma once

#include "PresentationOrientation.h"

namespace player_settings {
struct FloatSettingRange {
  float minimum, maximum, defaultValue;
};
struct PresentationGeometryPolicy {
  FloatSettingRange angle, length, width;
};
inline constexpr PresentationGeometryPolicy
presentationGeometryPolicy(PresentationOrientation orientation) {
  if (orientation == PresentationOrientation::Portrait) {
    return {{0.0F, 28.0F, 0.0F}, {4.0F, 32.0F, 16.0F}, {2.0F, 16.0F, 9.5F}};
  }
  return {{0.0F, 28.0F, 13.4F}, {5.0F, 12.0F, 8.0F}, {4.0F, 12.0F, 8.0F}};
}
} // namespace player_settings
