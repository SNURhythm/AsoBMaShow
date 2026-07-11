#pragma once

#include "../../audio/PlaybackRate.h"

#include <cmath>

namespace gameplay_timing {

struct PracticeFrameTiming {
  long long chartTimeMicros = 0;
  bool sectionComplete = false;
};

inline PracticeFrameTiming practiceFrameTiming(long long rawSongTimeMicros,
                                               long long audioOffsetMicros,
                                               long long endMicros) {
  const long long chartTimeMicros = rawSongTimeMicros + audioOffsetMicros;
  if (chartTimeMicros < endMicros) {
    return {.chartTimeMicros = chartTimeMicros};
  }
  return {
      .chartTimeMicros = endMicros - 1,
      .sectionComplete = true,
  };
}

inline long long visualTimeMicros(long long songTimeMicros,
                                  long long visualOffsetMicros) {
  return songTimeMicros - visualOffsetMicros;
}

inline long long realJudgementDiffMicros(long long chartDiffMicros,
                                         audio::PlaybackRate playback) {
  return playback.realMicrosFromChart(chartDiffMicros);
}

inline double leadInBeatDistance(long long targetTimeMicros,
                                 long long renderTimeMicros, double bpm) {
  if (renderTimeMicros >= targetTimeMicros || !std::isfinite(bpm) ||
      bpm <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(targetTimeMicros - renderTimeMicros) * bpm /
         240000000.0;
}

inline double playbackTravelScale(audio::PlaybackRate playback) {
  return playback.percent > 0 ? 100.0 / static_cast<double>(playback.percent)
                              : 1.0;
}

inline bool
shouldApplyPrepMetronome(bool settingEnabled,
                         unsigned long long practiceLeadInMicros,
                         long long startPositionMicros) {
  if (!settingEnabled) {
    return false;
  }
  if (practiceLeadInMicros == 0) {
    return true;
  }

  const auto availableLeadInMicros =
      startPositionMicros > 0
          ? static_cast<unsigned long long>(startPositionMicros)
          : 0ULL;
  return practiceLeadInMicros > availableLeadInMicros;
}

} // namespace gameplay_timing
