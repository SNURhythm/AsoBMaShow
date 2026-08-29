#pragma once

#include "ReplayData.h"
#include "bms_parser.hpp"
#include "scene/play/RhythmState.h"
#include "scene/play/SkinGameplayGraphState.h"

#include <optional>

namespace replay_result {
RhythmState BuildInitialGaugeState(
    bms_parser::Chart &chart, const ReplayData &replay,
    GaugeProfile gaugeProfile = GaugeProfile::Standard,
    const GaugeStateSnapshot *carriedGauge = nullptr);

RhythmState BuildResultState(bms_parser::Chart &chart,
                             const ReplayData &replay,
                             GaugeProfile gaugeProfile = GaugeProfile::Standard,
                             const GaugeStateSnapshot *carriedGauge = nullptr,
                             int carriedCombo = 0,
                             int carriedMaxCombo = 0);

// Saved results retain immutable chart data and replay events separately.
// Recreate the source state consumed by Beatoraja's note and timing graphs
// without substituting the score's gauge history for those independent data.
[[nodiscard]] SkinGameplayGraphState BuildSkinGameplayGraphState(
    bms_parser::Chart &chart, const ReplayData &replay,
    const RhythmState &state);

// A durable result can exist without its replay. Preserve chart-authored
// distributions and the captured gauge history in that case, while leaving
// replay-derived judgement and fast/slow distributions unavailable.
[[nodiscard]] SkinGameplayGraphState BuildSkinGameplayChartGraphState(
    bms_parser::Chart &chart, const RhythmState &state);

std::optional<long long> FindGaugeFailureMicros(
    bms_parser::Chart &chart, const ReplayData &replay,
    GaugeProfile gaugeProfile = GaugeProfile::Standard,
    const GaugeStateSnapshot *carriedGauge = nullptr);
}
