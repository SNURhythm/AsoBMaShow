#pragma once

#include "analysis/JudgedPlaybackResultState.h"

#include <algorithm>
#include <optional>

namespace replay_video_detail {

struct GaugeFailureProjection {
  std::optional<long long> failureMicros;
  JudgedPlaybackData resultReplay;
};

[[nodiscard]] inline GaugeFailureProjection projectGaugeFailure(
    bms_parser::Chart &chart, const JudgedPlaybackData &replay,
    const GaugeStateSnapshot *carriedGauge = nullptr) {
  GaugeFailureProjection projection{
      .failureMicros = analysis::FindGaugeFailureMicros(
          chart, replay, replay.setup.gaugeProfile, carriedGauge),
      .resultReplay = replay,
  };
  if (!projection.failureMicros.has_value()) {
    return projection;
  }

  auto &result = projection.resultReplay;
  result.events.erase(
      std::find_if(result.events.begin(), result.events.end(),
                   [&](const ReplayEvent &event) {
                     return event.songTimeMicros > *projection.failureMicros;
                   }),
      result.events.end());
  result.finalGauge = 0.0f;
  result.clearType = kClearTypeFailedRank;
  result.finalScore = result.events.empty() ? 0 : result.events.back().score;
  result.maxCombo = 0;
  for (const ReplayEvent &event : result.events) {
    result.maxCombo = std::max(result.maxCombo, event.combo);
  }
  return projection;
}

} // namespace replay_video_detail
