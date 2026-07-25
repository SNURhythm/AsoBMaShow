#pragma once

#include "JudgedPlaybackData.h"
#include "../bms_parser.hpp"
#include "../scene/play/RhythmState.h"

#include <optional>

namespace analysis {
RhythmState BuildInitialGaugeState(
    bms_parser::Chart &chart, const JudgedPlaybackData &replay,
    GaugeProfile gaugeProfile = GaugeProfile::Standard,
    const GaugeStateSnapshot *carriedGauge = nullptr);

RhythmState BuildResultState(bms_parser::Chart &chart,
                             const JudgedPlaybackData &replay,
                             GaugeProfile gaugeProfile = GaugeProfile::Standard,
                             const GaugeStateSnapshot *carriedGauge = nullptr,
                             int carriedCombo = 0,
                             int carriedMaxCombo = 0);

std::optional<long long> FindGaugeFailureMicros(
    bms_parser::Chart &chart, const JudgedPlaybackData &replay,
    GaugeProfile gaugeProfile = GaugeProfile::Standard,
    const GaugeStateSnapshot *carriedGauge = nullptr);
}
