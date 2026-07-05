#pragma once

#include <algorithm>

namespace touch_visualization_timing {

inline long long releaseElapsedMicros(bool released, long long releaseTimeMicros,
                                      long long currentTimeMicros) {
  if (!released) {
    return 0;
  }
  return std::max(0LL, currentTimeMicros - releaseTimeMicros);
}

inline bool shouldPruneReleasedTouch(bool released, long long releaseTimeMicros,
                                     long long currentTimeMicros,
                                     long long lingerMicros) {
  return releaseElapsedMicros(released, releaseTimeMicros, currentTimeMicros) >
         lingerMicros;
}

} // namespace touch_visualization_timing
