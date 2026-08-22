#pragma once

#include "../../audio/PlaybackRate.h"
#include "Judgement.h"

#include <cmath>
#include <optional>

namespace gameplay_timing {

struct PracticeFrameTiming {
  long long chartTimeMicros = 0;
  bool sectionComplete = false;
};

struct FrameTiming {
  long long rawSongTimeMicros = 0;
  long long gameplayTimeMicros = 0;
  long long bgaTimeMicros = 0;
  long long visualTimeMicros = 0;
};

inline long long gameplayTimeFromRawSongTime(long long rawSongTimeMicros,
                                             long long audioOffsetMicros) {
  return rawSongTimeMicros + audioOffsetMicros;
}

inline long long rawSongTimeFromGameplayTime(long long gameplayTimeMicros,
                                             long long audioOffsetMicros) {
  return gameplayTimeMicros - audioOffsetMicros;
}

inline std::optional<long long> rawSongTimeFromGameplayTime(
    const std::optional<long long> &gameplayTimeMicros,
    long long audioOffsetMicros) {
  if (!gameplayTimeMicros.has_value()) {
    return std::nullopt;
  }
  return rawSongTimeFromGameplayTime(*gameplayTimeMicros, audioOffsetMicros);
}

inline FrameTiming frameTiming(long long rawSongTimeMicros,
                               long long audioOffsetMicros,
                               long long visualOffsetMicros) {
  const long long gameplayTimeMicros =
      gameplayTimeFromRawSongTime(rawSongTimeMicros, audioOffsetMicros);
  return {
      .rawSongTimeMicros = rawSongTimeMicros,
      .gameplayTimeMicros = gameplayTimeMicros,
      .bgaTimeMicros = gameplayTimeMicros,
      .visualTimeMicros = gameplayTimeMicros - visualOffsetMicros,
  };
}

inline PracticeFrameTiming practiceFrameTiming(long long rawSongTimeMicros,
                                               long long audioOffsetMicros,
                                               long long endMicros) {
  const long long chartTimeMicros =
      gameplayTimeFromRawSongTime(rawSongTimeMicros, audioOffsetMicros);
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

inline long long noteDisplayTimeMicros(long long visualTimeMicros,
                                       int displayTimingMilliseconds) {
  return visualTimeMicros +
         static_cast<long long>(displayTimingMilliseconds) * 1'000LL;
}

// Exact JudgeManager notes-display timing auto-adjust. The source mutates
// PlayerConfig.judgetiming only for judge IDs 0 through 2 while PLAY or
// PRACTICE is active; Java's signed integer division truncates toward zero.
[[nodiscard]] inline int nextNotesDisplayTimingMilliseconds(
    int currentMilliseconds, bool enabled, bool playOrPractice,
    Judgement judgement, long long judgementDiffMicros) noexcept {
  if (!enabled || !playOrPractice || static_cast<int>(judgement) > 2 ||
      judgementDiffMicros < -150'000 || judgementDiffMicros > 150'000) {
    return currentMilliseconds;
  }
  const long long adjusted =
      judgementDiffMicros >= 0 ? judgementDiffMicros + 15'000
                               : judgementDiffMicros - 15'000;
  return currentMilliseconds - static_cast<int>(adjusted / 30'000);
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
