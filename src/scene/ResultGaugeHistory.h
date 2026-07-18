#pragma once

#include "play/GameplayScoreState.h"

#include <vector>

namespace result_gauge_history {

[[nodiscard]] std::vector<GaugeType>
availableTypes(const GameplayScoreState &state);

[[nodiscard]] GaugeType initialType(const GameplayScoreState &state);

[[nodiscard]] GaugeType nextType(const GameplayScoreState &state,
                                 GaugeType current);

[[nodiscard]] const std::vector<float> &
historyFor(const GameplayScoreState &state, GaugeType type);

} // namespace result_gauge_history
