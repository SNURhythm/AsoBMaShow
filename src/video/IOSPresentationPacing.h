#pragma once

#include <cmath>
#include <cstdint>

namespace video {
inline std::uint32_t iosPresentationPacingCap(float displayRefreshRate,
                                              std::uint32_t explicitFrameCap,
                                              bool exporting) {
  if (explicitFrameCap != 0 || exporting) {
    return 0;
  }
  if (!std::isfinite(displayRefreshRate) || displayRefreshRate < 1.0F ||
      displayRefreshRate > 1000.0F) {
    return 60;
  }
  return static_cast<std::uint32_t>(std::lround(displayRefreshRate));
}
} // namespace video
