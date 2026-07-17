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
} // namespace

int main() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 432;
  chart.Meta.KeyMode = 7;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 280.0;

  RhythmState legacy(&chart, false);
  GameplayScoreState runtime({.totalNotes = 432,
                              .keyMode = 7,
                              .gaugeTotal = 280.0});
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
