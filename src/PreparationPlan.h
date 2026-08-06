#pragma once

#include "PrepMetronome.h"
#include "audio/PlaybackRate.h"
#include "bms_parser.hpp"

#include <optional>
#include <vector>

namespace preparation {

inline constexpr long long kStartLaneIndicatorRealMicros = 2'000'000;

struct StartLaneIndicatorPlan {
  std::vector<int> lanes;
  long long startTimeMicros = 0;
  long long endTimeMicros = 0;

  [[nodiscard]] bool enabled() const noexcept {
    return !lanes.empty() && startTimeMicros < endTimeMicros;
  }

  [[nodiscard]] bool visibleAt(long long chartTimeMicros) const noexcept {
    return enabled() && chartTimeMicros >= startTimeMicros &&
           chartTimeMicros < endTimeMicros;
  }
};

struct Plan {
  prep_metronome::PrepMetronomePlan metronome;
  StartLaneIndicatorPlan laneIndicator;
  audio::PlaybackRate playback;
  long long playbackStartTimeMicros = 0;

  [[nodiscard]] bool indicatorVisibleAt(long long chartTimeMicros) const {
    return laneIndicator.visibleAt(chartTimeMicros);
  }

  [[nodiscard]] long long chartTimeAtRealTime(long long realTimeMicros) const {
    return playbackStartTimeMicros +
           playback.chartMicrosFromReal(realTimeMicros);
  }

  [[nodiscard]] long long realTimeAtChartTime(long long chartTimeMicros) const {
    return playback.realMicrosFromChart(chartTimeMicros -
                                        playbackStartTimeMicros);
  }

  [[nodiscard]] long long
  realTimeAtGameplayTime(long long gameplayTimeMicros,
                         long long audioOffsetMicros) const {
    return realTimeAtChartTime(gameplayTimeMicros - audioOffsetMicros);
  }

  // Beatoraja's skin-state clock starts when its READY state begins. In this
  // app, a prep metronome is the equivalent audible state boundary; do not
  // consume a skin's READY animation during an earlier lane-indicator cue.
  [[nodiscard]] long long skinAnimationStartTimeMicros() const noexcept {
    return metronome.enabled ? metronome.startTimeMicros
                             : playbackStartTimeMicros;
  }
};

std::vector<int>
firstPlayableLanes(const bms_parser::Chart &chart, long long startTimeMicros,
                   std::optional<long long> endTimeMicros = std::nullopt);

Plan buildNormalPlan(const bms_parser::Chart &chart, bool indicatorEnabled,
                     bool metronomeEnabled, long long playbackAnchorMicros,
                     long long noteRangeStartMicros,
                     std::optional<long long> noteRangeEndMicros,
                     audio::PlaybackRate playback);

Plan buildPracticePlan(const bms_parser::Chart &chart, bool indicatorEnabled,
                       long long practiceStartMicros,
                       long long practiceEndMicros, int countInBeats,
                       audio::PlaybackRate playback);

} // namespace preparation
