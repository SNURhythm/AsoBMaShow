#include "ReplaySetup.h"

#include "../bms_parser.hpp"
#include "../scene/play/GameplayAttemptSetup.h"

#include <algorithm>
#include <array>
#include <ranges>
#include <string_view>

namespace replay {
namespace {

bool canonicalHex(std::string_view value, std::size_t size) noexcept {
  return value.size() == size &&
         std::ranges::all_of(value, [](unsigned char ch) {
           return (ch >= '0' && ch <= '9') ||
                  (ch >= 'a' && ch <= 'f');
         });
}

bool supportedKeyMode(int keyMode) noexcept {
  constexpr std::array modes{5, 7, 9, 10, 14, 24, 48};
  return std::ranges::find(modes, keyMode) != modes.end();
}

bool enumBetween(int value, int first, int last) noexcept {
  return value >= first && value <= last;
}

std::string_view assignmentSymbols(int keyMode) noexcept {
  switch (keyMode) {
  case 5:
    return "S12345";
  case 7:
    return "S1234567";
  case 10:
    return "L123456789AR";
  case 14:
    return "L123456789ABCDER";
  default:
    return {};
  }
}

bool validManualOption(std::string_view option, int keyMode) noexcept {
  constexpr std::string_view prefix = "ASSIGN:";
  if (!option.starts_with(prefix)) {
    return false;
  }
  const std::string_view symbols = assignmentSymbols(keyMode);
  const std::string_view notation = option.substr(prefix.size());
  if (symbols.empty() || notation.size() != symbols.size()) {
    return false;
  }
  for (std::size_t index = 0; index < notation.size(); ++index) {
    if (!symbols.contains(notation[index]) ||
        notation.find(notation[index]) != index) {
      return false;
    }
  }
  return true;
}

bool validStockOption(std::string_view option) noexcept {
  constexpr std::array<std::string_view, 10> options{
      "NORMAL", "MIRROR",   "RANDOM",  "R-RANDOM",  "S-RANDOM",
      "SPIRAL", "H-RANDOM", "ALL-SCR", "RANDOM-EX", "S-RANDOM-EX",
  };
  return std::ranges::find(options, option) != options.end();
}

bool validOption(const ReplayPlayerOption &option, int keyMode,
                 const ReplayLimits &limits) noexcept {
  return option.option.size() <= limits.maxStringBytes &&
         (!option.seed.has_value() || *option.seed >= 0) &&
         (validStockOption(option.option) ||
          validManualOption(option.option, keyMode));
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

ReplaySetupValidation invalid(ReplaySetupIssue issue) noexcept {
  return {.issue = issue};
}

} // namespace

ReplaySetupValidation validateReplaySetup(
    const ReplaySetup &setup, ReplaySetupSource source,
    const ReplayLimits &limits) {
  if (source != ReplaySetupSource::LocalCapture &&
      source != ReplaySetupSource::AsoExtension &&
      source != ReplaySetupSource::StockBeatoraja) {
    return invalid(ReplaySetupIssue::Source);
  }
  if (!limits.valid()) {
    return invalid(ReplaySetupIssue::Limits);
  }
  if (!canonicalHex(setup.chart.sha256, 64)) {
    return invalid(ReplaySetupIssue::ChartSha256);
  }
  const bool md5Required = source != ReplaySetupSource::StockBeatoraja;
  if ((md5Required && !canonicalHex(setup.chart.md5, 32)) ||
      (!setup.chart.md5.empty() &&
       !canonicalHex(setup.chart.md5, 32))) {
    return invalid(ReplaySetupIssue::ChartMd5);
  }
  if (!supportedKeyMode(setup.chart.keyMode)) {
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
  const int doublePlayOption = static_cast<int>(setup.doublePlayOption);
  if (!enumBetween(doublePlayOption,
                   static_cast<int>(DoublePlayOption::Normal),
                   static_cast<int>(DoublePlayOption::Flip)) ||
      (setup.doublePlayOption == DoublePlayOption::Flip &&
       setup.chart.keyMode != 10 && setup.chart.keyMode != 14)) {
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
       setup.ruleset !=
           RulesetDescriptor::For(GameplayRuleset::Beatoraja))) {
    return invalid(ReplaySetupIssue::Ruleset);
  }
  if (!setup.playback.valid()) {
    return invalid(ReplaySetupIssue::PlaybackRate);
  }
  if (!enumBetween(
          static_cast<int>(setup.candidateSelection),
          static_cast<int>(gameplay::CandidateSelectionMode::LR2),
          static_cast<int>(gameplay::CandidateSelectionMode::Score))) {
    return invalid(ReplaySetupIssue::CandidateSelection);
  }
  if (!gameplay::validJudgeWindowScalePercent(
          setup.judgeWindowScalePercent)) {
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

ReplayChartMatch compareReplayChartIdentity(
    const ReplayChartIdentity &recorded,
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
