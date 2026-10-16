#pragma once

#include <cmath>
#include <cstdint>

namespace video {
inline std::uint32_t idlePresentationPacingCap(float displayRefreshRate,
                                               std::uint32_t explicitFrameCap,
                                               bool idle,
                                               bool vsyncEnabled = true) {
  if (!idle || (explicitFrameCap != 0 && !vsyncEnabled)) return 0;
  // Skipping bgfx::frame also skips its VSync wait. Keep service updates at
  // display cadence. The main loop's longer wait preserves lower explicit caps.
  if (!std::isfinite(displayRefreshRate) || displayRefreshRate < 1.0F ||
      displayRefreshRate > 1000.0F) return 120;
  return static_cast<std::uint32_t>(std::lround(displayRefreshRate));
}
} // namespace video
