#include "ReplaySetupProvenance.h"

#include "../scene/play/GameplayScoreState.h"

namespace replay {
namespace {

const ScoreStageProvenance *stageForSetup(
    const ReplaySetup &setup, const ScoreProvenance &provenance) noexcept {
  bms_parser::ChartMeta chart;
  chart.MD5 = setup.chart.md5;
  chart.SHA256 = setup.chart.sha256;
  return score_provenance::uniqueStageForChart(provenance, chart);
}

bool samePlayerOption(const ReplayPlayerOption &setup,
                      const PlayerOptionProvenance &provenance) noexcept {
  return setup.option == provenance.option && setup.seed == provenance.seed;
}

} // namespace

std::optional<ReplaySetup>
captureLocalReplaySetup(const LocalReplaySetupFacts &facts,
                        const ScoreProvenance &provenance,
                        std::string &diagnostic) noexcept {
  diagnostic.clear();
  try {
    ReplaySetup setup;
    setup.chart = facts.chart;
    setup.longNoteMode = facts.longNoteMode;
    setup.hasUndefinedLongNotes = facts.hasUndefinedLongNotes;

    const auto *stage = stageForSetup(setup, provenance);
    if (stage == nullptr || stage->longNoteMode != facts.longNoteMode) {
      diagnostic =
          "Result provenance does not identify one matching chart stage.";
      return std::nullopt;
    }

    setup.chartRandomSeed = stage->chartRandomSeed;
    setup.chartRandomPrng = stage->chartRandomPrng;
    setup.chartRandomValues = stage->chartRandomValues;
    setup.player1 = {.option = provenance.player1.option,
                     .seed = provenance.player1.seed,
                     .laneShufflePattern = facts.player1LaneShufflePattern};
    setup.player2 = {.option = provenance.player2.option,
                     .seed = provenance.player2.seed,
                     .laneShufflePattern = facts.player2LaneShufflePattern};
    setup.doublePlayOption = provenance.doublePlayFlip
                                 ? DoublePlayOption::Flip
                                 : DoublePlayOption::Normal;
    setup.assistOption = assist_options::normalize(provenance.assistOption);
    setup.initialGaugeType = provenance.gaugeType;
    setup.gaugeProfile = provenance.gaugeProfile;
    setup.gaugeAutoShift = provenance.gaugeAutoShift;
    setup.gaugeAutoShiftLowerBound = provenance.gaugeAutoShiftLowerBound;
    setup.ruleset = provenance.ruleset;
    setup.playback = provenance.playback;
    setup.candidateSelection = stage->candidateSelection;
    setup.judgeWindowScalePercent = provenance.judgeWindowScalePercent;
    setup.startingGaugePercent =
        provenance.startingGaugePercent.has_value()
            ? static_cast<float>(*provenance.startingGaugePercent)
            : gaugeInitialValue(provenance.gaugeType, provenance.gaugeProfile);
    setup.initialLaneCoverPercent = facts.initialLaneCoverPercent;
    setup.laneCoverEnabled = facts.laneCoverEnabled;
    setup.clubMode = provenance.clubMode;

    return setup;
  } catch (...) {
    diagnostic = "Captured replay setup construction failed.";
    return std::nullopt;
  }
}

bool replaySetupAgreesWithProvenance(
    const ReplaySetup &setup, const ScoreProvenance &provenance) noexcept {
  try {
    const auto *stage = stageForSetup(setup, provenance);
    if (stage == nullptr) {
      return false;
    }
    const bool startingGaugeAgrees =
        !provenance.startingGaugePercent.has_value() ||
        setup.startingGaugePercent ==
            static_cast<float>(*provenance.startingGaugePercent);
    return setup.longNoteMode == stage->longNoteMode &&
           setup.chartRandomSeed == stage->chartRandomSeed &&
           setup.chartRandomPrng == stage->chartRandomPrng &&
           setup.chartRandomValues == stage->chartRandomValues &&
           samePlayerOption(setup.player1, provenance.player1) &&
           samePlayerOption(setup.player2, provenance.player2) &&
           setup.doublePlayOption ==
               (provenance.doublePlayFlip ? DoublePlayOption::Flip
                                          : DoublePlayOption::Normal) &&
           setup.assistOption ==
               assist_options::normalize(provenance.assistOption) &&
           setup.initialGaugeType == provenance.gaugeType &&
           setup.gaugeProfile == provenance.gaugeProfile &&
           setup.gaugeAutoShift == provenance.gaugeAutoShift &&
           setup.gaugeAutoShiftLowerBound ==
               provenance.gaugeAutoShiftLowerBound &&
           setup.ruleset == provenance.ruleset &&
           setup.playback == provenance.playback &&
           setup.candidateSelection == stage->candidateSelection &&
           setup.judgeWindowScalePercent ==
               provenance.judgeWindowScalePercent &&
           setup.clubMode == provenance.clubMode && startingGaugeAgrees;
  } catch (...) {
    return false;
  }
}

} // namespace replay
