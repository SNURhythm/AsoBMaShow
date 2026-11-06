#pragma once

#include "PlayfieldVisualState.h"
#include "../../skin/beatoraja/GameplaySkinEndAnimation.h"

#include <algorithm>
#include <limits>

// Native playback retains original chart timestamps. Skin TIMER_PLAY uses
// Beatoraja's absolute frequency-adjusted chart clock, including preroll.
[[nodiscard]] inline PlayfieldPlayTimerAuthority
practiceSkinPlayTimer(long long chartTimeMicros, long long startMicros,
                     long long endMicros, bool menuActive,
                     int frequencyPercent = 100) noexcept {
  startMicros = std::max(0LL, startMicros);
  endMicros = std::max(startMicros, endMicros);
  const long long frequency = std::max(1, frequencyPercent);
  const auto durationMillis = std::clamp(
      (endMicros / 1000 + 1000) * 100 / frequency +
          skin::kBeatorajaEndOfNotesMarginMillis,
      1LL, static_cast<long long>(std::numeric_limits<std::int32_t>::max()));
  // Encode the source elapsed time in the existing clock pair without
  // changing the gameplay/replay chart-time authority.
  const auto elapsedMicros = chartTimeMicros / frequency * 100 +
                            chartTimeMicros % frequency * 100 / frequency;
  return {.active = !menuActive &&
                    chartTimeMicros >= std::max(0LL, startMicros - 1'000'000),
          .startMicros = chartTimeMicros - elapsedMicros,
          .elapsedMillisExact = true,
          .playtimeMillis = static_cast<std::int32_t>(durationMillis)};
}
