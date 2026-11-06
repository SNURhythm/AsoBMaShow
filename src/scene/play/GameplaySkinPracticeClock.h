#pragma once

#include "PlayfieldVisualState.h"
#include "../../skin/beatoraja/GameplaySkinEndAnimation.h"

#include <algorithm>
#include <limits>

// Practice presentation uses Aso's chart-time clock, including its playback
// rate and pauses. Pair elapsed time with the selected range in that same
// domain; Beatoraja's frequency-adjusted wall-clock formula cannot be mixed
// with this clock. This is presentation authority, not an attempt-end policy.
[[nodiscard]] inline PlayfieldPlayTimerAuthority
practiceSkinPlayTimer(long long chartTimeMicros, long long startMicros,
                     long long endMicros, bool menuActive) noexcept {
  startMicros = std::max(0LL, startMicros);
  endMicros = std::max(startMicros, endMicros);
  // The bridge subtracts this standard margin for its end-of-notes timer.
  const auto durationMillis = std::clamp(
      (endMicros - startMicros) / 1000 + skin::kBeatorajaEndOfNotesMarginMillis, 1LL,
      static_cast<long long>(std::numeric_limits<std::int32_t>::max()));
  return {.active = !menuActive && chartTimeMicros >= startMicros,
          .startMicros = startMicros,
          .elapsedMillisExact = true,
          .playtimeMillis = static_cast<std::int32_t>(durationMillis)};
}
