#include "ReplaySetupAdapter.h"

#include <limits>

namespace replay {

std::optional<ReplayData> makeReplayDataFromSetup(
    const ReplaySetup &setup, const ScoreProvenance &provenance,
    const bms_parser::ChartMeta &parsedChartMeta,
    std::string &diagnostic) noexcept {
  try {
    diagnostic.clear();
    if (setup.chartRandomSeed.has_value() &&
        *setup.chartRandomSeed > std::numeric_limits<unsigned int>::max()) {
      diagnostic = "Replay random seed cannot be represented by the parser.";
      return std::nullopt;
    }

    ReplayData result;
    result.chartMeta = parsedChartMeta;
    result.chartMeta.LnMode = setup.longNoteMode;
    if (setup.chartRandomSeed.has_value()) {
      result.randomSeed = static_cast<unsigned int>(*setup.chartRandomSeed);
    }
    result.randomPrng = setup.chartRandomPrng;
    result.randomValues = setup.chartRandomValues;
    result.playOption = setup.player1.option;
    result.playOptionSeed = setup.player1.seed;
    result.playOption2 = setup.player2.option;
    result.playOption2Seed = setup.player2.seed;
    result.assistOption = setup.assistOption;
    result.initialGaugeType = setup.initialGaugeType;
    result.gaugeAutoShift = setup.gaugeAutoShift;
    result.gaugeAutoShiftLowerBound = setup.gaugeAutoShiftLowerBound;
    result.provenance = provenance;
    return result;
  } catch (...) {
    diagnostic = "Replay setup translation failed.";
    return std::nullopt;
  }
}

} // namespace replay
