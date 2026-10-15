#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace video {
inline std::uint32_t iosPresentationPacingCap(float displayRefreshRate,
                                              std::uint32_t explicitFrameCap,
                                              bool exporting,
                                              bool gameplayActive = true) {
  if (exporting || (gameplayActive && explicitFrameCap != 0)) {
    return 0;
  }
  if (!std::isfinite(displayRefreshRate) || displayRefreshRate < 1.0F ||
      displayRefreshRate > 1000.0F) {
    return 60;
  }
  const auto displayCap =
      static_cast<std::uint32_t>(std::lround(displayRefreshRate));
  // A separate explicit pacer still enforces any lower user-selected cap.
  return gameplayActive ? displayCap : std::min(displayCap, 60U);
}
} // namespace video
