#pragma once

#include "GameplayScoreState.h"
#include "QuickRetryProjection.h"
#include <cmath>
#include <limits>
#include <span>

namespace quick_retry {
// Each unfinished condition is an alternative opportunity for progress.
struct Condition {
  int score = -1;
  int lamp = -1;
};

inline int maximumClear(const GameplayScoreState &state, int remaining,
                        const Projection &projection = {}) {
  if (state.activeGaugeFailed()) return kClearTypeFailedRank;
  int lower = gaugeTypeIndex(state.gaugeType), upper = lower;
  if (state.gaugeAutoShift == GaugeAutoShiftMode::BestClear ||
      state.gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder) {
    upper = state.gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder
                ? gaugeTypeIndex(state.selectedGaugeType) : gaugeTypeIndex(GaugeType::Hazard);
    lower = std::min(gaugeTypeIndex(state.gaugeAutoShiftLowerBound), upper);
  } else if (state.gaugeAutoShift == GaugeAutoShiftMode::SurvivalToGroove &&
             state.gaugeSurvivalFailed[lower]) {
    lower = upper = gaugeTypeIndex(GaugeType::Normal);
  }
  int best = kClearTypeFailedRank;
  const auto &rules = state.gaugeRules();
  for (int i = upper; i >= lower; --i) {
    if (state.gaugeSurvivalFailed[i]) continue;
    const auto type = gaugeTypeAtIndex(i);
    const auto &definition = rules.gauges[i];
    if (definition.survival && projection.pendingClassicJudgements[Bad] > 0 &&
        -rules.delta(type, Bad, 0) >= definition.maximum) continue;
    const float delta = rules.delta(type, PGreat, state.gaugeValues[i]);
    const float hcnDelta = rules.delta(type, Great, state.gaugeValues[i], 0.5F);
    double value = state.gaugeValues[i] + double(delta) * remaining +
                   double(hcnDelta) * projection.hellChargeTicks;
    int pending = 0;
    for (int j = Great; j <= Bad; ++j) {
      const int count = projection.pendingClassicJudgements[j];
      pending += count;
      value += count * double(rules.delta(type, static_cast<Judgement>(j),
                                         0) - delta);
    }
    // Resolve ordinary near-border cases with the scorer's float additions.
    // For mixed HCN/deferred outcomes, retain an optimistic rounding bound.
    const double error = (remaining + projection.hellChargeTicks) *
                         std::numeric_limits<float>::epsilon() * definition.maximum;
    if (projection.hellChargeTicks == 0 && pending == 0 &&
        std::abs(value - definition.clearBorder) <= error) {
      float exact = state.gaugeValues[i];
      for (int n = 0; n < remaining; ++n)
        exact = std::clamp(exact + delta, definition.minimum, definition.maximum);
      value = exact;
    } else if (projection.hellChargeTicks > 0 || pending > 0) {
      value += error;
    }
    value = std::min<double>(definition.maximum, value);
    if (value > 0 && value >= definition.clearBorder) {
      best = gaugeTypeToClearRank(gaugeClearTypeForProfile(type, state.gaugeProfile));
      break;
    }
  }
  if (best == kClearTypeFailedRank) return best;
  if (state.assistClearMark) best = kClearTypeAssistedEasyClearRank;
  else if (state.lightAssistClearMark) best = kClearTypeLightAssistedEasyClearRank;
  const int total = state.stagePassedNotes + remaining;
  const bool fullCombo = total > 0 && state.comboBreak == 0 &&
      projection.pendingClassicJudgements[Bad] == 0 &&
      std::max(state.maxCombo, state.combo + remaining) >= total;
  return clear_policy::fullComboRankForPlayback(best, fullCombo, {});
}

inline bool shouldRetry(const GameplayScoreState &state, int totalNotes,
                        std::span<const Condition> conditions,
                        const Projection &projection = {}) {
  if (conditions.empty() || totalNotes <= 0) return false;
  const int remaining = state.activeGaugeFailed() ? 0 :
      std::max(0, totalNotes - state.stagePassedNotes);
  auto maximumScore = std::int64_t(state.getScore()) + 2LL * remaining;
  if (!state.activeGaugeFailed()) {
    maximumScore -= projection.pendingClassicJudgements[Great] +
        2LL * (projection.pendingClassicJudgements[Good] + projection.pendingClassicJudgements[Bad]);
  }
  std::optional<int> lamp;
  for (const auto &condition : conditions) {
    if (condition.score >= 0 && maximumScore >= condition.score) return false;
    if (condition.lamp >= 0) {
      if (!lamp) lamp = maximumClear(state, remaining, projection);
      if (clearLampOrder(*lamp) >= clearLampOrder(condition.lamp)) return false;
    }
  }
  return true;
}
} // namespace quick_retry
