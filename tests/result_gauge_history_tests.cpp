#include "scene/ResultGaugeHistory.h"

#include "view/ClearLampColors.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bool sameColor(const Color &left, const Color &right) {
  return left.r == right.r && left.g == right.g && left.b == right.b &&
         left.a == right.a;
}

GameplayScoreState stateWithHistories() {
  GameplayScoreState state({});
  for (int index = 0; index < static_cast<int>(kGaugeTypeCount); ++index) {
    state.gaugeHistoryFor(gaugeTypeAtIndex(index)) = {
        static_cast<float>(index), static_cast<float>(index + 10)};
  }
  return state;
}

std::vector<std::size_t>
indices(std::span<const result_gauge_history::ResultGaugeGraphPoint> points) {
  std::vector<std::size_t> result;
  result.reserve(points.size());
  for (const auto &point : points) {
    result.push_back(point.index);
  }
  return result;
}

void testBestClearStartsWithAdoptedGaugeAndCyclesEverySeries() {
  auto state = stateWithHistories();
  state.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  state.gaugeAutoShiftLowerBound = GaugeType::Easy;
  state.gaugeType = GaugeType::Hard;

  const auto series = result_gauge_history::seriesFor(state);
  const std::vector<std::string> expectedLabels{"HARD", "HAZARD", "EX-HARD",
                                                "NORMAL", "EASY"};
  const std::vector<int> expectedRanks{
      kClearTypeHardClearRank, kClearTypeFullComboRank,
      kClearTypeExHardClearRank, kClearTypeNormalClearRank,
      kClearTypeEasyClearRank};
  require(series.size() == expectedLabels.size(),
          "Best Clear exposes every existing candidate");
  for (std::size_t index = 0; index < series.size(); ++index) {
    require(series[index].label == expectedLabels[index],
            "Best Clear keeps deterministic candidate labels");
    require(series[index].clearRank == expectedRanks[index],
            "Best Clear keeps the semantic color rank for each label");
    require(series[index].points.size() == 2 &&
                series[index].points.front().has_value(),
            "local float histories become present optional points");
  }
  require(series.front().label == "HARD" &&
              series.front().points.front() == 3.0F,
          "Best Clear starts with the final adopted gauge");

  std::size_t selected = 0;
  for (std::size_t expected = 1; expected < series.size(); ++expected) {
    selected = result_gauge_history::nextSeriesIndex(series, selected);
    require(selected == expected, "tap advances through every GAS series");
  }
  require(result_gauge_history::nextSeriesIndex(series, selected) == 0,
          "GAS series cycling wraps to the final adopted gauge");
}

void testSurvivalToGrooveAndNonGasSeriesStayCompatible() {
  auto state = stateWithHistories();
  state.gaugeAutoShift = GaugeAutoShiftMode::SurvivalToGroove;
  state.selectedGaugeType = GaugeType::ExHard;
  state.gaugeType = GaugeType::Normal;

  auto series = result_gauge_history::seriesFor(state);
  require(series.size() == 2 && series[0].label == "NORMAL" &&
              series[1].label == "EX-HARD",
          "Survival-to-Groove exposes adopted groove then selected survival");

  state.gaugeAutoShift = GaugeAutoShiftMode::None;
  state.gaugeType = GaugeType::Normal;
  series = result_gauge_history::seriesFor(state);
  require(series.size() == 1 && series.front().label == "NORMAL",
          "non-GAS exposes only the active gauge");

  for (auto &history : state.gaugeHistories) {
    history.clear();
  }
  state.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  state.gaugeType = GaugeType::Hard;
  state.gaugeHistory = {42.0F};
  series = result_gauge_history::seriesFor(state);
  require(series.size() == 1 && series.front().label == "HARD" &&
              series.front().points ==
                  std::vector<std::optional<float>>{
                      std::optional<float>{42.0F}},
          "legacy mixed history remains the final gauge as one present point");
}

void testNullRunsCreateSeparateStripsWithoutBridge() {
  const ResultGaugeSeries series{
      .points = {10.0F, 20.0F, std::nullopt, std::nullopt, 70.0F, 90.0F},
      .label = "REMOTE GAUGE",
      .clearRank = kClearTypeNormalClearRank,
  };
  const auto graph = result_gauge_history::graphFor(
      std::span<const ResultGaugeSeries>(&series, 1), 0);
  require(graph.has_value(), "a nullable history with values has graph geometry");
  require(graph->geometry.strips.size() == 2,
          "a null run divides gauge history into two strips");
  require(indices(graph->geometry.strips[0].points) ==
              std::vector<std::size_t>({0, 1}) &&
              indices(graph->geometry.strips[1].points) ==
                  std::vector<std::size_t>({4, 5}),
          "strip point indices preserve the null gap");
  require(graph->geometry.segments.size() == 2 &&
              graph->geometry.segments[0].from.index == 0 &&
              graph->geometry.segments[0].to.index == 1 &&
              graph->geometry.segments[1].from.index == 4 &&
              graph->geometry.segments[1].to.index == 5,
          "segments join only adjacent present points");
  require(indices(graph->geometry.markers) ==
              std::vector<std::size_t>({0, 1, 4, 5}),
          "markers are emitted only for present points");
}

void testEmptyAllNullAndSinglePresentGeometry() {
  const std::vector<ResultGaugeSeries> empty{{.points = {}}};
  require(!result_gauge_history::graphFor(empty, 0),
          "empty history has no graph geometry");

  const std::vector<ResultGaugeSeries> allNull{{
      .points = {std::nullopt, std::nullopt, std::nullopt},
      .label = "NORMAL",
  }};
  require(!result_gauge_history::graphFor(allNull, 0),
          "all-null history has no graph geometry");

  const std::vector<ResultGaugeSeries> single{{
      .points = {std::nullopt, 42.0F, std::nullopt},
      .label = "NORMAL",
  }};
  const auto graph = result_gauge_history::graphFor(single, 0);
  require(graph && graph->geometry.strips.size() == 1 &&
              graph->geometry.strips.front().points.size() == 1 &&
              graph->geometry.strips.front().points.front().index == 1 &&
              graph->geometry.segments.empty() &&
              graph->geometry.markers.size() == 1 &&
              graph->geometry.markers.front().index == 1,
          "one present value emits one marker and no segment");
  require(result_gauge_history::nextSeriesIndex({}, 17) == 0,
          "cycling an empty series list remains safely at zero");
}

void testSuppliedAndSemanticLabelsKeepCorrectColors() {
  const std::vector<ResultGaugeSeries> supplied{{
      .points = {50.0F},
      .label = "PROVIDER CUSTOM",
      .clearRank = kClearTypeHardClearRank,
  }};
  const auto suppliedGraph = result_gauge_history::graphFor(supplied, 0);
  require(suppliedGraph && suppliedGraph->label &&
              suppliedGraph->label->text == "PROVIDER CUSTOM" &&
              sameColor(suppliedGraph->label->background,
                        clearLampColorForRank(kClearTypeHardClearRank)),
          "supplied label is preserved while known lamp controls its color");

  const std::vector<ResultGaugeSeries> suppliedOnly{{
      .points = {50.0F},
      .label = "PROVIDER ONLY",
  }};
  const auto suppliedOnlyGraph =
      result_gauge_history::graphFor(suppliedOnly, 0);
  require(suppliedOnlyGraph && suppliedOnlyGraph->label &&
              suppliedOnlyGraph->label->text == "PROVIDER ONLY",
          "supplied label is preserved even without known local semantics");

  const std::vector<ResultGaugeSeries> lampOnly{{
      .points = {50.0F},
      .clearRank = kClearTypeNormalClearRank,
  }};
  const auto lampGraph = result_gauge_history::graphFor(lampOnly, 0);
  require(lampGraph && lampGraph->label &&
              lampGraph->label->text == "NORMAL CLEAR" &&
              sameColor(lampGraph->label->background,
                        clearLampColorForRank(kClearTypeNormalClearRank)),
          "known lamp semantics provide a lamp label without inventing a gauge");

  const std::vector<ResultGaugeSeries> knownGauge{{
      .points = {50.0F},
      .label = "NORMAL",
  }};
  const auto gaugeGraph = result_gauge_history::graphFor(knownGauge, 0);
  require(gaugeGraph && gaugeGraph->label &&
              gaugeGraph->label->text == "NORMAL" &&
              sameColor(gaugeGraph->label->background,
                        clearLampColorForRank(kClearTypeNormalClearRank)),
          "known supplied gauge semantics choose the matching label color");

  const std::vector<ResultGaugeSeries> unknown{{
      .points = {50.0F},
      .clearRank = 999,
  }};
  const auto unknownGraph = result_gauge_history::graphFor(unknown, 0);
  require(unknownGraph && !unknownGraph->label,
          "unknown metadata never invents a gauge or lamp label");
}
} // namespace

int main() {
  testBestClearStartsWithAdoptedGaugeAndCyclesEverySeries();
  testSurvivalToGrooveAndNonGasSeriesStayCompatible();
  testNullRunsCreateSeparateStripsWithoutBridge();
  testEmptyAllNullAndSinglePresentGeometry();
  testSuppliedAndSemanticLabelsKeepCorrectColors();
  return 0;
}
