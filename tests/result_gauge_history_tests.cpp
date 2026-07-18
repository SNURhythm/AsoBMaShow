#include "scene/ResultGaugeHistory.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

GameplayScoreState stateWithHistories() {
  GameplayScoreState state({});
  for (int index = 0; index < static_cast<int>(kGaugeTypeCount); ++index) {
    state.gaugeHistoryFor(gaugeTypeAtIndex(index)).push_back(
        static_cast<float>(index));
  }
  return state;
}

void testBestClearStartsWithAdoptedGaugeAndCycles() {
  auto state = stateWithHistories();
  state.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  state.gaugeAutoShiftLowerBound = GaugeType::Easy;
  state.gaugeType = GaugeType::Hard;

  const auto types = result_gauge_history::availableTypes(state);
  require(types == std::vector<GaugeType>({GaugeType::Hard,
                                           GaugeType::Hazard,
                                           GaugeType::ExHard,
                                           GaugeType::Normal,
                                           GaugeType::Easy}),
          "Best Clear starts with the adopted gauge then orders candidates");
  require(result_gauge_history::initialType(state) == GaugeType::Hard,
          "initial result gauge is the final adopted gauge");
  require(result_gauge_history::nextType(state, GaugeType::Hard) ==
              GaugeType::Hazard,
          "tap advances to the next GAS candidate");
  require(result_gauge_history::nextType(state, GaugeType::Easy) ==
              GaugeType::Hard,
          "gauge candidate cycling wraps to the adopted gauge");
}

void testSurvivalToGrooveShowsOnlyRelevantGauges() {
  auto state = stateWithHistories();
  state.gaugeAutoShift = GaugeAutoShiftMode::SurvivalToGroove;
  state.selectedGaugeType = GaugeType::ExHard;
  state.gaugeType = GaugeType::Normal;

  require(result_gauge_history::availableTypes(state) ==
              std::vector<GaugeType>({GaugeType::Normal, GaugeType::ExHard}),
          "Survival-to-Groove exposes adopted groove and selected survival");
}

void testEmptyAndNonGasSeriesStaySingleGauge() {
  auto state = stateWithHistories();
  state.gaugeAutoShift = GaugeAutoShiftMode::None;
  state.gaugeType = GaugeType::Normal;
  require(result_gauge_history::availableTypes(state) ==
              std::vector<GaugeType>({GaugeType::Normal}),
          "non-GAS exposes only the active gauge");

  state.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  state.gaugeType = GaugeType::Hard;
  for (auto &history : state.gaugeHistories) {
    history.clear();
  }
  state.gaugeHistory = {42.0F};
  require(result_gauge_history::availableTypes(state) ==
              std::vector<GaugeType>({GaugeType::Hard}),
          "legacy mixed history falls back to the final gauge");
}
} // namespace

int main() {
  testBestClearStartsWithAdoptedGaugeAndCycles();
  testSurvivalToGrooveShowsOnlyRelevantGauges();
  testEmptyAndNonGasSeriesStaySingleGauge();
  return 0;
}
