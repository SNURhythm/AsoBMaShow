#include "ReplaySetup.h"

#include "ReplayFormat.h"
#include "ReplayKeyMode.h"
#include "ReplayOption.h"

#include "../bms_parser.hpp"
#include "../scene/play/GameplayAttemptSetup.h"

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
  if (!result_contract::isValidKeyMode(setup.chart.keyMode) ||
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

} // namespace replay
