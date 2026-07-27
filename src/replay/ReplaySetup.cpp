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
  if (!isCanonicalLowerHex(setup.chart.sha256, 64)) {
    return invalid(ReplaySetupIssue::ChartSha256);
  }
  const bool md5Required = source != ReplaySetupSource::StockBeatoraja;
  if ((md5Required && !isCanonicalLowerHex(setup.chart.md5, 32)) ||
      (!setup.chart.md5.empty() && !isCanonicalLowerHex(setup.chart.md5, 32))) {
    return invalid(ReplaySetupIssue::ChartMd5);
  }
  if (!replayKeyModeLayout(setup.chart.keyMode)) {
    return invalid(ReplaySetupIssue::KeyMode);
  }
  if (setup.longNoteMode < 0 || setup.longNoteMode > 3 ||
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
  if (!enumBetween(static_cast<int>(setup.initialGaugeType),
                   static_cast<int>(GaugeType::AssistedEasy),
                   static_cast<int>(GaugeType::Hazard))) {
    return invalid(ReplaySetupIssue::GaugeType);
  }
  if (!enumBetween(static_cast<int>(setup.gaugeProfile),
                   static_cast<int>(GaugeProfile::Standard),
                   static_cast<int>(GaugeProfile::Standard24Keys))) {
    return invalid(ReplaySetupIssue::GaugeProfile);
  }
  if (!enumBetween(static_cast<int>(setup.gaugeAutoShift),
                   static_cast<int>(GaugeAutoShiftMode::None),
                   static_cast<int>(GaugeAutoShiftMode::BestClear))) {
    return invalid(ReplaySetupIssue::GaugeAutoShift);
  }
  if (!enumBetween(static_cast<int>(setup.gaugeAutoShiftLowerBound),
                   static_cast<int>(GaugeType::AssistedEasy),
                   static_cast<int>(GaugeType::Hazard))) {
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
  if (recorded.sha256 != selected.sha256) {
    return ReplayChartMatch::Sha256Mismatch;
  }
  if (!recorded.md5.empty() && recorded.md5 != selected.md5) {
    return ReplayChartMatch::Md5Mismatch;
  }
  if (recorded.keyMode != selected.keyMode) {
    return ReplayChartMatch::KeyModeMismatch;
  }
  return ReplayChartMatch::Match;
}

} // namespace replay
