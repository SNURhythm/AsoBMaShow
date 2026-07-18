#pragma once
#include "../../bms_parser.hpp"
#include "GameplayScoreState.h"

class RhythmState : public GameplayScoreState {
public:
  explicit RhythmState(
      const bms_parser::Chart *chart, bool addReadyMeasure,
      GameplayRuleset ruleset = GameplayRuleset::Beatoraja,
      GaugeProfile profile = GaugeProfile::Standard)
      : GameplayScoreState(configFor(chart, ruleset, profile)), chart_(chart),
        ruleset_(ruleset) {
    (void)addReadyMeasure;
  }

  void configureGauge(GaugeType newSelectedGaugeType,
                      GaugeAutoShiftMode autoShift,
                      GaugeProfile selectedGaugeProfile =
                          GaugeProfile::Standard,
                      GaugeType autoShiftLowerBound =
                          GaugeType::AssistedEasy) {
    setGaugeRules(
        compileGameplayGaugeRules(ruleset_, chartMeta(),
                                  selectedGaugeProfile),
        chart_ == nullptr ? 7 : chart_->Meta.KeyMode);
    GameplayScoreState::configureGauge(newSelectedGaugeType, autoShift,
                                       selectedGaugeProfile,
                                       autoShiftLowerBound);
  }

private:
  [[nodiscard]] bms_parser::ChartMeta chartMeta() const {
    return chart_ == nullptr ? bms_parser::ChartMeta{} : chart_->Meta;
  }

  static GameplayScoreConfig configFor(const bms_parser::Chart *chart,
                                       GameplayRuleset ruleset,
                                       GaugeProfile profile) {
    const bms_parser::ChartMeta meta =
        chart == nullptr ? bms_parser::ChartMeta{} : chart->Meta;
    return {
        .gaugeRules = compileGameplayGaugeRules(ruleset, meta, profile),
        .keyMode = meta.KeyMode,
    };
  }

  const bms_parser::Chart *chart_ = nullptr;
  GameplayRuleset ruleset_ = GameplayRuleset::Beatoraja;
};
