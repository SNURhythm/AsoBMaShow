#include "bms_parser.hpp"
#include "scene/play/GameplayScoreState.h"

#include <bit>
#include <cstdint>
#include <iostream>

int main() {
  int ruleset, profile, gauge, notes, hasTotal, start, judge, steps;
  double total;
  float rate, mine;
  while (std::cin >> ruleset >> profile >> gauge >> notes >> total >> hasTotal >>
         start >> judge >> rate >> mine >> steps) {
    bms_parser::ChartMeta meta;
    meta.TotalNotes = notes;
    meta.Total = total;
    meta.HasTotal = hasTotal;
    meta.KeyMode = 7;
    const auto rules = compileGameplayGaugeRules(
        static_cast<GameplayRuleset>(ruleset), meta,
        static_cast<GaugeProfile>(profile));
    const auto type = gaugeTypeAtIndex(gauge);
    const auto &definition = rules.gauges[gauge];
    std::cout << std::bit_cast<std::uint64_t>(rules.effectiveTotal);
    for (const float value : {definition.initial, definition.minimum,
                              definition.maximum, definition.clearBorder,
                              definition.deathBelow}) {
      std::cout << ' ' << std::bit_cast<std::uint32_t>(value);
    }
    for (const float value : definition.baseDelta) {
      std::cout << ' ' << std::bit_cast<std::uint32_t>(value);
    }
    GameplayScoreState state({.gaugeRules = rules, .keyMode = 7});
    state.configureGauge(type, GaugeAutoShiftMode::None);
    state.setStartingGaugePercent(start);
    const auto emitState = [&] {
      std::cout << ' ' << std::bit_cast<std::uint32_t>(state.currentGauge)
                << ' ' << (state.getClearType() != ClearType::Failed);
    };
    emitState();
    constexpr Judgement judgements[]{PGreat, Great, Good, Bad, Poor, Kpoor};
    for (int step = 0; step < steps; ++step) {
      if (judge < 0 || (judge == 6 && step % 7 == 6)) {
        state.applyGaugeDelta(mine);
      } else {
        state.applyGaugeJudgementRate(
            judgements[judge == 6 ? step % 7 : judge], rate);
      }
      emitState();
    }
    std::cout << '\n';
  }
}
