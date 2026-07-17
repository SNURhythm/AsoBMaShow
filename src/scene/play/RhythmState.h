#pragma once
#include "../../bms_parser.hpp"
#include "GameplayScoreState.h"

class RhythmState : public GameplayScoreState {
public:
  explicit RhythmState(const bms_parser::Chart *chart, bool addReadyMeasure)
      : GameplayScoreState(configFor(chart)) {
    (void)addReadyMeasure;
  }

private:
  static GameplayScoreConfig configFor(const bms_parser::Chart *chart) {
    if (chart == nullptr) {
      return {};
    }
    return {
        .totalNotes = chart->Meta.TotalNotes,
        .keyMode = chart->Meta.KeyMode,
        .gaugeTotal =
            chart->Meta.HasTotal
                ? chart->Meta.Total
                : beatorajaDefaultGaugeTotal(chart->Meta.KeyMode,
                                             chart->Meta.TotalNotes),
    };
  }
};
