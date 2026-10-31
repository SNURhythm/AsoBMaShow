#pragma once

#include "GameplayScoreState.h"
#include "QuickRetryProjection.h"
#include <limits>
#include <span>

namespace quick_retry {
// Each unfinished condition is an alternative opportunity for progress.
struct Condition {
  int score = -1;
  int lamp = -1;
};

struct Targets {
  // Unfinished goal conditions are alternatives; the other requirements are independent.
  int score = -1;
  int lamp = -1;
  int pacemakerScore = -1;
  bool requireGaugeClear = false;
  bool allConditionsFail = false;
  bool empty() const noexcept { return score < 0 && lamp < 0 && pacemakerScore < 0 && !requireGaugeClear; }
  void add(Condition condition) {
    if (condition.score >= 0 && (score < 0 || condition.score < score)) score = condition.score;
    if (condition.lamp >= 0 && (lamp < 0 || clearLampOrder(condition.lamp) < clearLampOrder(lamp)))
      lamp = normalizeClearLamp(condition.lamp);
  }
};

inline Targets compileTargets(std::span<const Condition> conditions) {
  Targets targets;
  for (const auto &condition : conditions) targets.add(condition);
  return targets;
}

inline bool canClearGauge(const GameplayScoreState &state, GaugeType type, int remaining,
                          const Projection &projection = {}) {
  const int i = gaugeTypeIndex(type);
  if (state.gaugeSurvivalFailed[i]) return false;
  const auto &rules = state.gaugeRules();
  const auto &definition = rules.gauges[i];
  if (definition.survival && projection.pendingClassicJudgements[Bad] > 0 &&
      -rules.delta(type, Bad, 0) >= definition.maximum) return false;
  const float delta = rules.delta(type, PGreat, state.gaugeValues[i]);
  const float hcnDelta = rules.delta(type, Great, state.gaugeValues[i], 0.5F);
  double recovery = double(delta) * remaining +
                    double(hcnDelta) * projection.hellChargeTicks;
  double damage = 0;
  for (int j = Great; j <= Bad; ++j) {
    const int count = projection.pendingClassicJudgements[j];
    const double deferredDelta = rules.delta(type, static_cast<Judgement>(j), 0);
    recovery += count * (std::max(0.0, deferredDelta) - delta);
    damage += count * std::min(0.0, deferredDelta);
  }
  // Damage before recovery maximizes the benefit of a groove gauge's floor.
  // This optimistic ordering needs only cached judgement counts, not a scan
  // of pending tails. Survival gauges cannot recover through their death floor.
  double value = state.gaugeValues[i] + damage;
  if (!definition.survival) value = std::max<double>(definition.minimum, value);
  value += recovery;
  // A conservative float accumulation bound avoids replaying every remaining
  // judgement near the border. Work is bounded by the fixed gauge table.
  value += (remaining + projection.hellChargeTicks) *
           std::numeric_limits<float>::epsilon() * definition.maximum;
  value = std::min<double>(definition.maximum, value);
  return value > 0 && value >= definition.clearBorder;
}

inline bool canClearRequiredGauge(const GameplayScoreState &state, int remaining,
                                  const Projection &projection = {}) {
  auto required = state.selectedGaugeType;
  if (state.gaugeAutoShift == GaugeAutoShiftMode::BestClear ||
      state.gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder) {
    const int upper = state.gaugeAutoShift == GaugeAutoShiftMode::SelectToUnder
                          ? gaugeTypeIndex(state.selectedGaugeType)
                          : gaugeTypeIndex(GaugeType::Hazard);
    required = gaugeTypeAtIndex(std::min(gaugeTypeIndex(state.gaugeAutoShiftLowerBound), upper));
  }
  if (canClearGauge(state, required, remaining, projection)) return true;
  return state.gaugeAutoShift == GaugeAutoShiftMode::SurvivalToGroove &&
         state.gaugeRules().gauges[gaugeTypeIndex(required)].survival &&
         canClearGauge(state, GaugeType::Normal, remaining, projection);
}

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
             state.gaugeRules().gauges[lower].survival &&
             !canClearGauge(state, state.gaugeType, remaining, projection)) {
    lower = upper = gaugeTypeIndex(GaugeType::Normal);
  }
  int best = kClearTypeFailedRank;
  for (int i = upper; i >= lower; --i) {
    if (state.gaugeSurvivalFailed[i]) continue;
    const auto type = gaugeTypeAtIndex(i);
    if (canClearGauge(state, type, remaining, projection)) {
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
                        const Targets &targets,
                        const Projection &projection = {}) {
  if (targets.empty() || totalNotes <= 0) return false;
  const int remaining = state.activeGaugeFailed() ? 0 :
      std::max(0, totalNotes - state.stagePassedNotes);
  bool anyFailed = false;
  bool allFailed = true;
  const auto include = [&](bool failed) {
    anyFailed |= failed;
    allFailed &= failed;
  };
  if (targets.requireGaugeClear) include(!canClearRequiredGauge(state, remaining, projection));
  auto maximumScore = std::int64_t(state.getScore()) + 2LL * remaining;
  if (!state.activeGaugeFailed()) {
    maximumScore -= projection.pendingClassicJudgements[Great] +
        2LL * (projection.pendingClassicJudgements[Good] + projection.pendingClassicJudgements[Bad]);
  }
  if (targets.pacemakerScore >= 0) include(maximumScore < targets.pacemakerScore);
  if (targets.score >= 0 || targets.lamp >= 0) {
    const bool scoreReachable = targets.score >= 0 && maximumScore >= targets.score;
    const bool lampReachable = targets.lamp >= 0 &&
        clearLampOrder(maximumClear(state, remaining, projection)) >= clearLampOrder(targets.lamp);
    include(!scoreReachable && !lampReachable);
  }
  return targets.allConditionsFail ? allFailed : anyFailed;
}

inline Targets armReachableTargets(const GameplayScoreState &state, int totalNotes,
                                   Targets targets, const Projection &projection = {}) {
  // An impossible starting target must not cause a restart loop or disable an
  // independent requirement that can still be achieved with these settings.
  if (shouldRetry(state, totalNotes, {.score = targets.score, .lamp = targets.lamp}, projection))
    targets.score = targets.lamp = -1;
  if (shouldRetry(state, totalNotes, {.pacemakerScore = targets.pacemakerScore}, projection))
    targets.pacemakerScore = -1;
  if (targets.requireGaugeClear &&
      shouldRetry(state, totalNotes, {.requireGaugeClear = true}, projection))
    targets.requireGaugeClear = false;
  return targets;
}
} // namespace quick_retry
