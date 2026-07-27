#include "ReplaySetup.h"

#include "ReplayFormat.h"
#include "ReplayKeyMode.h"
#include "ReplayOption.h"

#include "../ScoreProvenance.h"
#include "../bms_parser.hpp"
#include "../scene/play/GameplayAttemptSetup.h"
#include "../scene/play/GameplayScoreState.h"

#include <algorithm>
#include <ranges>

namespace replay {
namespace {

bool enumBetween(int value, int first, int last) noexcept {
  return value >= first && value <= last;
}

bool validOption(const ReplayPlayerOption &option, int keyMode,
                 const ReplayLimits &limits) noexcept {
  return option.option.size() <= limits.maxStringBytes &&
         (!option.seed.has_value() || *option.seed >= 0) &&
         validReplayPlayerOptionName(option.option, keyMode);
}

bool optionsCompatible(const ReplayPlayerOption &first,
                       const ReplayPlayerOption &second) noexcept {
  const bool firstManual = first.option.starts_with("ASSIGN:");
  const bool secondManual = second.option.starts_with("ASSIGN:");
  if (firstManual && secondManual) {
    return first.option == second.option;
  }
  if (firstManual) {
    return second.option == "NORMAL";
  }
  if (secondManual) {
    return first.option == "NORMAL";
  }
  return true;
}

bool validLaneShufflePattern(const ReplayPlayerOption &option,
                             int keyMode) noexcept {
  if (!option.laneShufflePattern.has_value()) {
    return true;
  }
  const auto &pattern = *option.laneShufflePattern;
  const auto layout = replayKeyModeLayout(keyMode);
  if (!layout ||
      pattern.size() != static_cast<std::size_t>(layout->stockShuffleWidth)) {
    return false;
  }
  const std::size_t expected =
      static_cast<std::size_t>(layout->stockShuffleWidth);
  std::vector<bool> seen(expected, false);
  for (const int lane : pattern) {
    if (lane < 0 || static_cast<std::size_t>(lane) >= expected ||
        seen[static_cast<std::size_t>(lane)]) {
      return false;
    }
    seen[static_cast<std::size_t>(lane)] = true;
  }
  return true;
}

ReplaySetupValidation invalid(ReplaySetupIssue issue) noexcept {
  return {.issue = issue};
}

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

ReplaySetupValidation validateReplaySetup(const ReplaySetup &setup,
                                          ReplaySetupSource source,
                                          const ReplayLimits &limits) {
  if (source != ReplaySetupSource::LocalCapture &&
      source != ReplaySetupSource::AsoExtension &&
      source != ReplaySetupSource::StockBeatoraja) {
    return invalid(ReplaySetupIssue::Source);
  }
  if (!limits.valid()) {
    return invalid(ReplaySetupIssue::Limits);
  }
  if (!result_contract::canonicalChartHashes({}, setup.chart.sha256, false)) {
    return invalid(ReplaySetupIssue::ChartSha256);
  }
  const bool md5Required = source != ReplaySetupSource::StockBeatoraja;
  if (!result_contract::canonicalChartHashes(setup.chart.md5,
                                             setup.chart.sha256, md5Required)) {
    return invalid(ReplaySetupIssue::ChartMd5);
  }
  if (!result_contract::isSupportedKeyMode(setup.chart.keyMode) ||
      !replayKeyModeLayout(setup.chart.keyMode)) {
    return invalid(ReplaySetupIssue::KeyMode);
  }
  if (!result_contract::isKnownLongNoteMode(setup.longNoteMode) ||
      (setup.hasUndefinedLongNotes && setup.longNoteMode == 0)) {
    return invalid(ReplaySetupIssue::LongNoteMode);
  }
  if (setup.chartRandomValues.size() > limits.maxRandomValues) {
    return invalid(ReplaySetupIssue::RandomValues);
  }
  if (setup.chartRandomPrng.has_value() &&
      (*setup.chartRandomPrng != bms_parser::Parser::RandomPrngId ||
       setup.chartRandomPrng->size() > limits.maxStringBytes)) {
    return invalid(ReplaySetupIssue::RandomPrng);
  }
  if (!validOption(setup.player1, setup.chart.keyMode, limits)) {
    return invalid(ReplaySetupIssue::PlayerOneOption);
  }
  if (!validOption(setup.player2, setup.chart.keyMode, limits)) {
    return invalid(ReplaySetupIssue::PlayerTwoOption);
  }
  if (!optionsCompatible(setup.player1, setup.player2)) {
    return invalid(ReplaySetupIssue::PlayerOptions);
  }
  if (!validLaneShufflePattern(setup.player1, setup.chart.keyMode) ||
      !validLaneShufflePattern(setup.player2, setup.chart.keyMode)) {
    return invalid(ReplaySetupIssue::LaneShufflePattern);
  }
  const int doublePlayOption = static_cast<int>(setup.doublePlayOption);
  const auto keyModeLayout = replayKeyModeLayout(setup.chart.keyMode);
  if (!enumBetween(doublePlayOption, static_cast<int>(DoublePlayOption::Normal),
                   static_cast<int>(DoublePlayOption::Flip)) ||
      (setup.doublePlayOption == DoublePlayOption::Flip &&
       (!keyModeLayout || !keyModeLayout->supportsDoublePlayFlip))) {
    return invalid(ReplaySetupIssue::DoublePlayOption);
  }
  if (setup.assistOption.size() > limits.maxStringBytes ||
      assist_options::normalize(setup.assistOption) != setup.assistOption) {
    return invalid(ReplaySetupIssue::AssistOption);
  }
  if (!result_contract::isKnownGaugeType(setup.initialGaugeType)) {
    return invalid(ReplaySetupIssue::GaugeType);
  }
  if (!result_contract::isKnownGaugeProfile(setup.gaugeProfile)) {
    return invalid(ReplaySetupIssue::GaugeProfile);
  }
  if (!result_contract::isKnownGaugeAutoShift(setup.gaugeAutoShift)) {
    return invalid(ReplaySetupIssue::GaugeAutoShift);
  }
  if (!result_contract::isKnownGaugeType(setup.gaugeAutoShiftLowerBound)) {
    return invalid(ReplaySetupIssue::GaugeAutoShiftLowerBound);
  }
  if (!isSupportedRulesetDescriptor(setup.ruleset) ||
      (source == ReplaySetupSource::StockBeatoraja &&
       setup.ruleset != RulesetDescriptor::For(GameplayRuleset::Beatoraja))) {
    return invalid(ReplaySetupIssue::Ruleset);
  }
  if (!setup.playback.valid()) {
    return invalid(ReplaySetupIssue::PlaybackRate);
  }
  if (!enumBetween(static_cast<int>(setup.candidateSelection),
                   static_cast<int>(gameplay::CandidateSelectionMode::LR2),
                   static_cast<int>(gameplay::CandidateSelectionMode::Score))) {
    return invalid(ReplaySetupIssue::CandidateSelection);
  }
  if (!gameplay::validJudgeWindowScalePercent(setup.judgeWindowScalePercent)) {
    return invalid(ReplaySetupIssue::JudgeWindowScale);
  }
  if (!gameplay::validStartingGaugePercent(setup.startingGaugePercent)) {
    return invalid(ReplaySetupIssue::StartingGauge);
  }
  if (setup.initialLaneCoverPercent < 0 ||
      setup.initialLaneCoverPercent > 100) {
    return invalid(ReplaySetupIssue::InitialLaneCover);
  }
  return {};
}

ReplayChartMatch
compareReplayChartIdentity(const ReplayChartIdentity &recorded,
                           const ReplayChartIdentity &selected) noexcept {
  return result_contract::compareChartIdentity(recorded, selected);
}

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
                     .laneShufflePattern =
                         facts.player1LaneShufflePattern};
    setup.player2 = {.option = provenance.player2.option,
                     .seed = provenance.player2.seed,
                     .laneShufflePattern =
                         facts.player2LaneShufflePattern};
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
            : gaugeInitialValue(provenance.gaugeType,
                                provenance.gaugeProfile);
    setup.initialLaneCoverPercent = facts.initialLaneCoverPercent;
    setup.laneCoverEnabled = facts.laneCoverEnabled;
    setup.clubMode = provenance.clubMode;

    const auto validation =
        validateReplaySetup(setup, ReplaySetupSource::LocalCapture);
    if (!validation.valid()) {
      diagnostic = "Captured replay setup is invalid (issue " +
                   std::to_string(static_cast<int>(validation.issue)) + ").";
      return std::nullopt;
    }
    if (!replaySetupAgreesWithProvenance(setup, provenance)) {
      diagnostic = "Captured replay setup differs from result provenance.";
      return std::nullopt;
    }
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
