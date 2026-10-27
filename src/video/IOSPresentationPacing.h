#pragma once

#include "FramePacer.h"

#include <algorithm>
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

inline std::chrono::steady_clock::duration iosFramePacingWait(
    FramePacer &explicitPacer, FramePacer &presentationPacer,
    std::chrono::steady_clock::time_point frameStartedAt,
    std::chrono::steady_clock::time_point submissionFinishedAt,
    bool displayLinkClocked = false) {
  // Drawable acquisition can already block for a refresh. Count that wait
  // and rendering against this frame's budget, rather than starting another
  // interval when submission returns. Idle frames still wait the remainder.
  explicitPacer.framePresented(frameStartedAt);
  presentationPacer.framePresented(frameStartedAt);
  if (displayLinkClocked) return std::chrono::steady_clock::duration::zero();
  return std::max(explicitPacer.remaining(submissionFinishedAt),
                  presentationPacer.remaining(submissionFinishedAt));
}
} // namespace video
