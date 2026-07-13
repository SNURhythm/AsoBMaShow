#pragma once

#include "ReplayData.h"
#include "bms_parser.hpp"
#include "scene/play/RhythmState.h"

#include <optional>

namespace replay_result {
RhythmState BuildInitialGaugeState(
    bms_parser::Chart &chart, const ReplayData &replay,
    GaugeProfile gaugeProfile = GaugeProfile::Standard,
    const GaugeStateSnapshot *carriedGauge = nullptr);

RhythmState BuildResultState(bms_parser::Chart &chart,
                             const ReplayData &replay,
                             GaugeProfile gaugeProfile = GaugeProfile::Standard,
                             const GaugeStateSnapshot *carriedGauge = nullptr);

std::optional<long long> FindGaugeFailureMicros(
    bms_parser::Chart &chart, const ReplayData &replay,
    GaugeProfile gaugeProfile = GaugeProfile::Standard,
    const GaugeStateSnapshot *carriedGauge = nullptr);
}
