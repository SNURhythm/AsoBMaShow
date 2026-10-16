#pragma once

#include <cmath>
#include <cstdint>

namespace video {
inline std::uint32_t idlePresentationPacingCap(float displayRefreshRate,
                                               std::uint32_t explicitFrameCap,
                                               bool idle) {
  if (!idle || explicitFrameCap != 0) return 0;
  // Skipping bgfx::frame also skips its VSync wait. Keep service updates at
  // display cadence without changing active rendering or explicit user caps.
  if (!std::isfinite(displayRefreshRate) || displayRefreshRate < 1.0F ||
      displayRefreshRate > 1000.0F) return 120;
  return static_cast<std::uint32_t>(std::lround(displayRefreshRate));
}
} // namespace video
