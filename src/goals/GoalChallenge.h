#pragma once
#include "ChartGoal.h"
#include "../scene/play/GameplayGaugeTypes.h"
#include <span>

namespace chart_goals {
inline GaugeType challengeGauge(GaugeType selected, GaugeAutoShiftMode shift, int lamp) {
  lamp = normalizeClearLamp(lamp);
  // Best Clear admits every gauge up to Hazard. The other modes never raise
  // the upper bound above the selected gauge (Survival to Groove falls down).
  if (lamp <= gaugeTypeToClearRank(selected) || shift == GaugeAutoShiftMode::BestClear)
    return selected;
  for (auto gauge : {GaugeType::AssistedEasy, GaugeType::Easy, GaugeType::Normal,
                     GaugeType::Hard, GaugeType::ExHard, GaugeType::Hazard}) {
    if (gaugeTypeToClearRank(gauge) >= lamp) return gauge;
  }
  return selected;
}

inline std::optional<Target> scoreTargetForGoals(std::span<const GoalProgress> goals,
                                                int mode, std::int64_t preferredId) {
  const Target *lowest = nullptr;
  for (const auto &entry : goals) {
    const auto &goal = entry.goal;
    if (goal.longNoteMode != mode) continue;
    if (preferredId > 0 && goal.id == preferredId)
      return goal.target.scoreNumerator > 0 ? std::optional{goal.target} : std::nullopt;
    if (goal.target.scoreNumerator <= 0 || entry.progress.scoreMet) continue;
    if (!lowest || static_cast<std::int64_t>(goal.target.scoreNumerator) * lowest->scoreDenominator <
        static_cast<std::int64_t>(lowest->scoreNumerator) * goal.target.scoreDenominator)
      lowest = &goal.target;
  }
  return lowest ? std::optional{*lowest} : std::nullopt;
}
} // namespace chart_goals
