#pragma once

#include "JudgedPlaybackData.h"
#include "../bms_parser.hpp"
#include "../scene/play/RhythmState.h"

#include <optional>

namespace analysis {
RhythmState BuildInitialGaugeState(
    bms_parser::Chart &chart, const JudgedPlaybackData &replay,
    std::optional<GaugeProfile> gaugeProfile = std::nullopt,
    const GaugeStateSnapshot *carriedGauge = nullptr);

RhythmState BuildResultState(bms_parser::Chart &chart,
                             const JudgedPlaybackData &replay,
                             std::optional<GaugeProfile> gaugeProfile =
                                 std::nullopt,
                             const GaugeStateSnapshot *carriedGauge = nullptr,
                             int carriedCombo = 0,
                             int carriedMaxCombo = 0);

struct ValidatedJudgedPlaybackResultState {
  RhythmState state;
  // Schema-v10 migration retained only samples whose recorded gauge type
  // equals the final adopted type. Keep that compatibility projection
  // distinct from GameplayScoreState's all-gauge runtime histories.
  std::vector<float> adoptedGaugeHistory;
};

// Reconstructs migration-only result facts while requiring every annotation's
// chart identity, timing, counters, and gauge snapshot to match the recorded
// gameplay transition.
std::optional<ValidatedJudgedPlaybackResultState> BuildValidatedResultState(
    bms_parser::Chart &chart, const JudgedPlaybackData &replay,
    std::optional<GaugeProfile> gaugeProfile = std::nullopt,
    const GaugeStateSnapshot *carriedGauge = nullptr, int carriedCombo = 0,
    int carriedMaxCombo = 0);

std::optional<long long> FindGaugeFailureMicros(
    bms_parser::Chart &chart, const JudgedPlaybackData &replay,
    std::optional<GaugeProfile> gaugeProfile = std::nullopt,
    const GaugeStateSnapshot *carriedGauge = nullptr);
}
