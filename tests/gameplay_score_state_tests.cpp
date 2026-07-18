#include "scene/play/GameplayScoreState.h"
#include "scene/play/RhythmState.h"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

GameplayScoreConfig beatorajaScoreConfig(int totalNotes, int keyMode,
                                         double total) {
  bms_parser::ChartMeta meta;
  meta.TotalNotes = totalNotes;
  meta.KeyMode = keyMode;
  meta.HasTotal = true;
  meta.Total = total;
  return {.gaugeRules = compileGameplayGaugeRules(
              GameplayRuleset::Beatoraja, meta, GaugeProfile::Standard),
          .keyMode = keyMode};
}

void requireSame(const GameplayScoreState &left,
                 const GameplayScoreState &right) {
  require(left.judgeCount == right.judgeCount, "judge counts match");
  require(left.judgementFastSlowCount == right.judgementFastSlowCount,
          "fast/slow counts match");
  require(left.combo == right.combo && left.maxCombo == right.maxCombo &&
              left.comboBreak == right.comboBreak,
          "combo state matches");
  require(left.gaugeType == right.gaugeType &&
              left.gaugeValues == right.gaugeValues &&
              left.gaugeSurvivalFailed == right.gaugeSurvivalFailed &&
              std::bit_cast<std::uint32_t>(left.currentGauge) ==
                  std::bit_cast<std::uint32_t>(right.currentGauge),
          "gauge state is bit-identical");
}

void testConfiguredGaugeHistoryUsesLogicalLimit() {
  GameplayScoreState state(beatorajaScoreConfig(10, 7, 100.0));
  state.gaugeHistory.reserve(8);
  state.configureBoundedGaugeHistory(2);
  const auto *const storage = state.gaugeHistory.data();
  const auto allocatedCapacity = state.gaugeHistory.capacity();
  require(allocatedCapacity >= 8,
          "fixture has allocator capacity above the logical limit");

  state.applyGaugeDelta(1.0F);
  state.applyGaugeDelta(1.0F);
  require(state.gaugeHistory.size() == 2 &&
              !state.gaugeHistoryOverflowed(),
          "configured history accepts exactly its logical limit");

  state.applyGaugeDelta(1.0F);
  require(state.gaugeHistory.size() == 2 &&
              state.gaugeHistoryOverflowed() &&
              state.gaugeHistory.data() == storage &&
              state.gaugeHistory.capacity() == allocatedCapacity,
          "logical history overflow latches without storage growth");
}

void testRhythmStateGaugeHistoryRemainsUnboundedByDefault() {
  bms_parser::Chart chart;
  RhythmState state(&chart, false);
  state.gaugeHistory.reserve(1);
  state.applyGaugeDelta(1.0F);
  state.applyGaugeDelta(1.0F);
  state.applyGaugeDelta(1.0F);
  require(state.gaugeHistory.size() == 3 &&
              !state.gaugeHistoryOverflowed(),
          "default RhythmState compatibility keeps growable gauge history");
}
} // namespace

int main() {
  testConfiguredGaugeHistoryUsesLogicalLimit();
  testRhythmStateGaugeHistoryRemainsUnboundedByDefault();

  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 432;
  chart.Meta.KeyMode = 7;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 280.0;

  RhythmState legacy(&chart, false);
  GameplayScoreState runtime(beatorajaScoreConfig(432, 7, 280.0));
  legacy.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::BestClear);
  runtime.configureGauge(GaugeType::Hard, GaugeAutoShiftMode::BestClear);
  legacy.configureBoundedGaugeHistory(64);
  runtime.configureBoundedGaugeHistory(64);

  for (const auto judge : {PGreat, Great, Good, Bad, Poor, Kpoor}) {
    legacy.commitJudge(JudgeResult(judge, judge == PGreat ? -10 : 10));
    runtime.commitJudge(JudgeResult(judge, judge == PGreat ? -10 : 10));
  }
  legacy.applyGaugeJudgementRate(Great, 0.5F);
  runtime.applyGaugeJudgementRate(Great, 0.5F);
  legacy.applyGaugeDelta(-3.25F);
  runtime.applyGaugeDelta(-3.25F);
  requireSame(legacy, runtime);
  require(legacy.getScore() == runtime.getScore(), "EX score matches");
  require(legacy.getClearTypeRank() == runtime.getClearTypeRank(),
          "clear state matches");
  return 0;
}
