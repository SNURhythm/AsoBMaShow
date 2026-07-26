#pragma once

#include "ArchiveFile.h"
#include "CoursePlaySession.h"
#include "analysis/JudgedPlaybackData.h"
#include "replay/LegacyReplayIdentity.h"
#include "replay/ReplayPlaybackData.h"
#include "bms_parser.hpp"
#include "path.h"

#include <SDL2/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace play_options {

struct PlayOptionReplayInfo {
  std::optional<std::string> option;
  std::optional<long long> seed;
  std::optional<std::string> option2;
  std::optional<long long> seed2;
};

struct PlayModeDisplayLabel {
  std::string mode;
  std::string laneOrder;
};

inline constexpr std::array<const char *, 10> kPlayOptions = {
    "NORMAL", "MIRROR",   "RANDOM",  "R-RANDOM",  "S-RANDOM",
    "SPIRAL", "H-RANDOM", "ALL-SCR", "RANDOM-EX", "S-RANDOM-EX"};

inline void trimOptionWhitespace(std::string &value) {
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), [](unsigned char c) {
                return std::isspace(c) == 0;
              }));
  value.erase(
      std::find_if(value.rbegin(), value.rend(),
                   [](unsigned char c) { return std::isspace(c) == 0; })
          .base(),
      value.end());
}

inline std::string normalizeLaneAssignNotation(std::string notation) {
  trimOptionWhitespace(notation);

  std::transform(notation.begin(), notation.end(), notation.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::toupper(c));
                 });

  constexpr std::array<std::string_view, 4> prefixes = {
      "ASSIGN:", "ASSIGN-", "MANUAL:", "MANUAL-"};
  for (std::string_view prefix : prefixes) {
    if (notation.rfind(prefix, 0) == 0) {
      notation.erase(0, prefix.size());
      break;
    }
  }

  std::string result;
  result.reserve(notation.size());
  bool hasDigit = false;
  for (unsigned char c : notation) {
    if (std::isspace(c) || c == '-' || c == '_' || c == '/' || c == ':') {
      continue;
    }
    const char upper = static_cast<char>(std::toupper(c));
    hasDigit = hasDigit || (upper >= '1' && upper <= '9');
    result.push_back(upper);
  }
  return hasDigit ? result : "";
}

inline std::optional<std::string>
laneAssignNotationFromOption(const std::string &option) {
  const std::string normalized = normalizeLaneAssignNotation(option);
  if (normalized.empty()) {
    return std::nullopt;
  }
  for (char c : normalized) {
    if (c != 'S' && c != 'L' && c != 'R' && (c < '1' || c > '9') &&
        (c < 'A' || c > 'E')) {
      return std::nullopt;
    }
  }
  return normalized;
}

inline std::string makeLaneAssignOption(const std::string &notation) {
  const auto normalized = laneAssignNotationFromOption(notation);
  return normalized.has_value() ? "ASSIGN:" + *normalized : "NORMAL";
}

inline std::string defaultLaneAssignNotation(
    const bms_parser::ChartMeta &meta) {
  constexpr std::string_view keySymbols = "123456789ABCDE";
  const auto keyLanes = meta.GetKeyLaneIndices();
  const auto scratchLanes = meta.GetScratchLaneIndices();
  std::string notation;
  if (meta.IsDP && scratchLanes.size() >= 2) {
    notation.push_back('L');
  } else if (!meta.IsDP && !scratchLanes.empty()) {
    notation.push_back('S');
  }
  notation.append(keySymbols.substr(0, std::min(keyLanes.size(),
                                                keySymbols.size())));
  if (meta.IsDP && scratchLanes.size() >= 2) {
    notation.push_back('R');
  }
  return notation;
}

inline bool validateLaneAssignOption(const bms_parser::ChartMeta &meta,
                                     const std::string &option,
                                     std::string *error = nullptr) {
  const auto notation = laneAssignNotationFromOption(option);
  if (!notation.has_value()) {
    if (error != nullptr) {
      *error = "Invalid lane assign notation.";
    }
    return false;
  }
  return bms_parser::ValidateLaneAssignNotation(meta, *notation, error);
}

inline std::string normalizePlayOption(std::string option) {
  trimOptionWhitespace(option);
  std::transform(option.begin(), option.end(), option.begin(),
                 [](unsigned char c) {
                   if (c == '_' || c == ' ') {
                     return '-';
                   }
                   return static_cast<char>(std::toupper(c));
                 });
  if (option == "OFF") {
    return "NORMAL";
  }
  for (const char *candidate : kPlayOptions) {
    if (option == candidate) {
      return option;
    }
  }
  if (const auto notation = laneAssignNotationFromOption(option);
      notation.has_value()) {
    return "ASSIGN:" + *notation;
  }
  return "NORMAL";
}

inline bool isNormalPlayOption(const std::string &option) {
  return normalizePlayOption(option) == "NORMAL";
}

inline bool usesRandomizer(const std::string &option) {
  const std::string normalized = normalizePlayOption(option);
  return normalized == "RANDOM" || normalized == "R-RANDOM" ||
         normalized == "S-RANDOM" || normalized == "SPIRAL" ||
         normalized == "H-RANDOM" || normalized == "ALL-SCR" ||
         normalized == "RANDOM-EX" || normalized == "S-RANDOM-EX";
}

inline bool usesRandomizer(const std::optional<std::string> &option) {
  return option.has_value() && usesRandomizer(*option);
}

inline bool isLaneShuffleOption(const std::optional<std::string> &option) {
  const std::string normalized =
      option.has_value() ? normalizePlayOption(*option) : "NORMAL";
  return normalized == "MIRROR" || normalized == "RANDOM" ||
         normalized == "R-RANDOM" || normalized == "RANDOM-EX";
}

inline std::optional<std::string>
formatLaneOrderSummary(const bms_parser::ChartMeta &meta,
                       const std::vector<int> &laneOrder) {
  const std::vector<int> destinationLanes = meta.GetTotalLaneIndices();
  if (destinationLanes.empty() || laneOrder.size() != destinationLanes.size()) {
    return std::nullopt;
  }

  std::unordered_map<int, char> laneToSymbol;
  const auto scratchLanes = meta.GetScratchLaneIndices();
  if (meta.IsDP) {
    if (scratchLanes.size() >= 2) {
      laneToSymbol[scratchLanes.front()] = 'L';
      laneToSymbol[scratchLanes.back()] = 'R';
    }
  } else if (!scratchLanes.empty()) {
    laneToSymbol[scratchLanes.front()] = 'S';
  }

  constexpr std::string_view keySymbols = "123456789ABCDE";
  const auto keyLanes = meta.GetKeyLaneIndices();
  if (keyLanes.size() > keySymbols.size()) {
    return std::nullopt;
  }
  for (size_t i = 0; i < keyLanes.size(); ++i) {
    laneToSymbol[keyLanes[i]] = keySymbols[i];
  }

  std::string result;
  result.reserve(laneOrder.size());
  for (int sourceLane : laneOrder) {
    const auto symbol = laneToSymbol.find(sourceLane);
    if (symbol == laneToSymbol.end()) {
      return std::nullopt;
    }
    result.push_back(symbol->second);
  }
  return result;
}

inline std::optional<std::vector<int>>
laneOrderForPlayOption(const bms_parser::ChartMeta &meta,
                       const std::optional<std::string> &option,
                       const std::optional<long long> &seed, int player) {
  const std::string normalized =
      option.has_value() ? normalizePlayOption(*option) : "NORMAL";
  if (usesRandomizer(normalized) && !seed.has_value()) {
    return std::nullopt;
  }

  auto modifier = bms_parser::CreatePlayOptionModifier(
      normalized, seed.value_or(-1), player);
  if (modifier == nullptr) {
    return std::nullopt;
  }

  const auto totalLanes = meta.GetTotalLaneIndices();
  if (totalLanes.empty()) {
    return std::nullopt;
  }
  const int laneCount = *std::max_element(totalLanes.begin(), totalLanes.end()) + 1;
  if (laneCount <= 0) {
    return std::nullopt;
  }

  bms_parser::Chart syntheticChart;
  syntheticChart.Meta = meta;
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(laneCount, false);
  measure->TimeLines.push_back(timeline);
  syntheticChart.Measures.push_back(measure);
  modifier->Modify(syntheticChart);
  return modifier->GetLaneOrder(meta);
}

inline std::optional<std::string> formatLaneShuffleSummary(
    const bms_parser::ChartMeta &meta, const std::optional<std::string> &option,
    const std::optional<long long> &seed,
    const std::optional<std::string> &option2 = std::nullopt,
    const std::optional<long long> &seed2 = std::nullopt) {
  if (!isLaneShuffleOption(option) &&
      (!meta.IsDP || !isLaneShuffleOption(option2))) {
    return std::nullopt;
  }

  const std::vector<int> identityLaneOrder = meta.GetTotalLaneIndices();
  std::vector<int> combinedLaneOrder = identityLaneOrder;
  auto mergeLaneOrder = [&](const std::vector<int> &laneOrder) {
    if (laneOrder.size() != combinedLaneOrder.size() ||
        laneOrder.size() != identityLaneOrder.size()) {
      return;
    }
    for (size_t i = 0; i < laneOrder.size(); ++i) {
      if (laneOrder[i] != identityLaneOrder[i]) {
        combinedLaneOrder[i] = laneOrder[i];
      }
    }
  };

  if (isLaneShuffleOption(option)) {
    const auto laneOrder = laneOrderForPlayOption(meta, option, seed, 0);
    if (!laneOrder.has_value()) {
      return std::nullopt;
    }
    mergeLaneOrder(*laneOrder);
  }
  if (meta.IsDP && isLaneShuffleOption(option2)) {
    const auto laneOrder = laneOrderForPlayOption(meta, option2, seed2, 1);
    if (!laneOrder.has_value()) {
      return std::nullopt;
    }
    mergeLaneOrder(*laneOrder);
  }

  return formatLaneOrderSummary(meta, combinedLaneOrder);
}

inline bool hasSamePatternRandomization(
    const bms_parser::ChartMeta &meta,
    const std::optional<std::string> &option = std::nullopt,
    const std::optional<std::string> &option2 = std::nullopt) {
  return !meta.RandomValues.empty() || usesRandomizer(option) ||
         usesRandomizer(option2);
}

inline bool hasSamePatternRandomization(const JudgedPlaybackData &replay) {
  return !replay.randomValues.empty() ||
         hasSamePatternRandomization(replay.chartMeta, replay.playOption,
                                     replay.playOption2);
}

inline std::string
formatPlayOptionLabel(const std::optional<std::string> &option,
                      const std::optional<long long> &seed = std::nullopt,
                      const std::optional<std::string> &option2 = std::nullopt,
                      const std::optional<long long> &seed2 = std::nullopt) {
  if (option.has_value()) {
    if (const auto assign = laneAssignNotationFromOption(*option);
        assign.has_value()) {
      return "ASSIGN " + *assign;
    }
  }
  const std::string normalizedOption =
      option.has_value() ? normalizePlayOption(*option) : "NORMAL";
  const std::string normalizedOption2 =
      option2.has_value() ? normalizePlayOption(*option2) : "NORMAL";
  const bool hasOption = !isNormalPlayOption(normalizedOption);
  const bool hasOption2 =
      option2.has_value() && !isNormalPlayOption(normalizedOption2);
  if (!hasOption && !hasOption2) {
    return "";
  }

  auto formatSingle = [](const std::string &name,
                         const std::optional<long long> &optionSeed) {
    std::string label = name;
    if (optionSeed.has_value()) {
      label += " #" + std::to_string(*optionSeed);
    }
    return label;
  };

  if (hasOption2) {
    return formatSingle(normalizedOption, seed) + " / " +
           formatSingle(normalizedOption2, seed2);
  }
  return formatSingle(normalizedOption, seed);
}

inline PlayModeDisplayLabel formatPlayModeDisplayLabel(
    const bms_parser::ChartMeta &meta, const std::optional<std::string> &option,
    const std::optional<long long> &seed = std::nullopt,
    const std::optional<std::string> &option2 = std::nullopt,
    const std::optional<long long> &seed2 = std::nullopt) {
  if (option.has_value()) {
    if (const auto assign = laneAssignNotationFromOption(*option);
        assign.has_value()) {
      return {.mode = "ASSIGN", .laneOrder = *assign};
    }
  }

  PlayModeDisplayLabel display{
      .mode = formatPlayOptionLabel(option, seed, option2, seed2)};
  if (display.mode.empty()) {
    display.mode = "NORMAL";
  }

  if (const auto laneSummary =
          formatLaneShuffleSummary(meta, option, seed, option2, seed2);
      laneSummary.has_value()) {
    display.laneOrder = *laneSummary;
  }
  return display;
}

inline PlayModeDisplayLabel formatPlayModeDisplayLabel(
    const JudgedPlaybackData &replay) {
  return formatPlayModeDisplayLabel(
      replay.chartMeta, replay.playOption, replay.playOptionSeed,
      replay.playOption2, replay.playOption2Seed);
}

inline std::string formatPlayModeLabel(
    const bms_parser::ChartMeta &meta, const std::optional<std::string> &option,
    const std::optional<long long> &seed = std::nullopt,
    const std::optional<std::string> &option2 = std::nullopt,
    const std::optional<long long> &seed2 = std::nullopt) {
  const PlayModeDisplayLabel display =
      formatPlayModeDisplayLabel(meta, option, seed, option2, seed2);
  return display.laneOrder.empty() ? display.mode
                                   : display.mode + " Lane " + display.laneOrder;
}

inline std::string formatPlayModeLabel(const JudgedPlaybackData &replay) {
  const PlayModeDisplayLabel display = formatPlayModeDisplayLabel(replay);
  return display.laneOrder.empty() ? display.mode
                                   : display.mode + " Lane " + display.laneOrder;
}

inline bool
applyPlayOptionModifier(bms_parser::Chart &chart, const std::string &option,
                        std::optional<long long> seed, int player,
                        std::optional<std::string> &appliedOption,
                        std::optional<long long> &appliedSeed,
                        std::string_view logContext = "play option",
                        std::vector<int> *appliedLaneOrder = nullptr) {
  const std::string normalized = normalizePlayOption(option);
  if (normalized.empty() || normalized == "NORMAL") {
    return true;
  }
  if (laneAssignNotationFromOption(normalized).has_value()) {
    std::string error;
    if (!validateLaneAssignOption(chart.Meta, normalized, &error)) {
      SDL_Log("Invalid %.*s modifier %s: %s",
              static_cast<int>(logContext.size()), logContext.data(),
              normalized.c_str(), error.c_str());
      return false;
    }
  }

  auto modifier = bms_parser::CreatePlayOptionModifier(
      normalized, seed.value_or(-1), player);
  if (modifier == nullptr) {
    SDL_Log("Unsupported %.*s modifier: %s",
            static_cast<int>(logContext.size()), logContext.data(),
            normalized.c_str());
    return false;
  }

  modifier->Modify(chart);
  appliedOption = modifier->Name();
  appliedSeed = usesRandomizer(*appliedOption)
                    ? std::optional<long long>(modifier->GetSeed())
                    : std::nullopt;
  if (appliedLaneOrder != nullptr) {
    *appliedLaneOrder = modifier->GetLaneOrder(chart.Meta);
  }
  return true;
}

inline bool applyReplayPlayOptions(bms_parser::Chart &chart,
                                   const JudgedPlaybackData &replay) {
  std::optional<std::string> ignoredOption;
  std::optional<long long> ignoredSeed;
  if (replay.playOption.has_value() &&
      !applyPlayOptionModifier(chart, *replay.playOption, replay.playOptionSeed,
                               0, ignoredOption, ignoredSeed, "replay")) {
    return false;
  }
  if (chart.Meta.IsDP && replay.playOption2.has_value() &&
      !applyPlayOptionModifier(chart, *replay.playOption2,
                               replay.playOption2Seed, 1, ignoredOption,
                               ignoredSeed, "replay")) {
    return false;
  }
  return true;
}

inline bool applyReplayPlayOptions(
    bms_parser::Chart &chart, const replay::ReplayPlaybackData &playback) {
  std::optional<std::string> ignoredOption;
  std::optional<long long> ignoredSeed;
  const auto &setup = playback.setup;
  if (setup.playOption.has_value() &&
      !applyPlayOptionModifier(chart, *setup.playOption,
                               setup.playOptionSeed, 0, ignoredOption,
                               ignoredSeed, "raw replay")) {
    return false;
  }
  if (chart.Meta.IsDP && setup.playOption2.has_value() &&
      !applyPlayOptionModifier(chart, *setup.playOption2,
                               setup.playOption2Seed, 1, ignoredOption,
                               ignoredSeed, "raw replay")) {
    return false;
  }
  return true;
}

inline PlayOptionReplayInfo
applySelectedPlayOptions(bms_parser::Chart &chart, const std::string &option) {
  PlayOptionReplayInfo info;
  if (!applyPlayOptionModifier(chart, option, std::nullopt, 0, info.option,
                               info.seed)) {
    return {};
  }
  if (chart.Meta.IsDP &&
      !laneAssignNotationFromOption(info.option.value_or("")).has_value() &&
      !applyPlayOptionModifier(chart, option, std::nullopt, 1, info.option2,
                               info.seed2)) {
    return {};
  }
  return info;
}

inline std::optional<std::vector<int>>
randomValuesOrNull(const std::vector<int> &randomValues) {
  if (randomValues.empty()) {
    return std::nullopt;
  }
  return randomValues;
}

inline void assignParsedChartPathMetadata(bms_parser::Chart &chart,
                                          const std::filesystem::path &path) {
  chart.Meta.BmsPath = path;
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (archive_file::splitVirtualPath(path, archivePath, innerPath)) {
    chart.Meta.Folder =
        archive_file::makeVirtualPath(archivePath, innerPath.parent_path());
  } else {
    chart.Meta.Folder = path.parent_path();
  }
}

inline bool configureParserRandom(
    bms_parser::Parser &parser, const std::optional<unsigned int> &randomSeed,
    const std::optional<std::string> &randomPrng,
    const std::optional<std::vector<int>> &randomValues,
    std::string_view logContext) {
  if (randomPrng.has_value() && !parser.SetRandomPrng(*randomPrng)) {
    SDL_Log("Unsupported %.*s random PRNG: %s",
            static_cast<int>(logContext.size()), logContext.data(),
            randomPrng->c_str());
    archive_file::appendDebugLogLine("Unsupported " + std::string(logContext) +
                                     " random PRNG: " + *randomPrng);
    return false;
  }
  if (randomSeed.has_value()) {
    parser.SetRandomSeed(*randomSeed);
  }
  if (randomValues.has_value()) {
    parser.SetRandomValues(*randomValues);
  }
  return true;
}

inline std::unique_ptr<bms_parser::Chart> parseChartBytes(
    const std::filesystem::path &path, const std::vector<unsigned char> &bytes,
    const std::optional<unsigned int> &randomSeed,
    const std::optional<std::string> &randomPrng,
    const std::optional<std::vector<int>> &randomValues,
    std::atomic_bool &cancelled, std::string_view logContext = "chart") {
  if (path.empty()) {
    SDL_Log("Cannot parse %.*s: chart path is empty",
            static_cast<int>(logContext.size()), logContext.data());
    archive_file::appendDebugLogLine(
        "Cannot parse " + std::string(logContext) + ": chart path is empty");
    return nullptr;
  }

  bms_parser::Parser parser;
  if (!configureParserRandom(parser, randomSeed, randomPrng, randomValues,
                             logContext)) {
    return nullptr;
  }

  bms_parser::Chart *parsedChart = nullptr;
  const std::string pathText = fspath_to_utf8(path);
  archive_file::appendDebugLogLine("Parse " + std::string(logContext) +
                                   " from bytes: " + pathText);
  parser.Parse(bytes, &parsedChart, false, false, cancelled);
  if (parsedChart != nullptr) {
    assignParsedChartPathMetadata(*parsedChart, path);
    archive_file::appendDebugLogLine(
        "Parse complete " + std::string(logContext) + ": " + pathText +
        " measures=" + std::to_string(parsedChart->Measures.size()));
  } else if (cancelled.load()) {
    archive_file::appendDebugLogLine("Parse cancelled " +
                                     std::string(logContext) + ": " +
                                     pathText);
  } else {
    archive_file::appendDebugLogLine("Parse returned null " +
                                     std::string(logContext) + ": " +
                                     pathText);
  }
  return std::unique_ptr<bms_parser::Chart>(parsedChart);
}

inline std::unique_ptr<bms_parser::Chart>
parseChart(const std::filesystem::path &path,
           const std::optional<unsigned int> &randomSeed,
           const std::optional<std::string> &randomPrng,
           const std::optional<std::vector<int>> &randomValues,
           std::atomic_bool &cancelled, std::string_view logContext = "chart") {
  if (path.empty()) {
    SDL_Log("Cannot parse %.*s: chart path is empty",
            static_cast<int>(logContext.size()), logContext.data());
    archive_file::appendDebugLogLine(
        "Cannot parse " + std::string(logContext) + ": chart path is empty");
    return nullptr;
  }

  bms_parser::Parser parser;
  if (!configureParserRandom(parser, randomSeed, randomPrng, randomValues,
                             logContext)) {
    return nullptr;
  }

  bms_parser::Chart *parsedChart = nullptr;
  const std::string pathText = fspath_to_utf8(path);
  archive_file::appendDebugLogLine("Parse " + std::string(logContext) + ": " +
                                   pathText);
  archive_file::parseChart(parser, path, &parsedChart, false, false,
                           cancelled);
  if (parsedChart != nullptr) {
    archive_file::appendDebugLogLine(
        "Parse complete " + std::string(logContext) + ": " + pathText +
        " measures=" + std::to_string(parsedChart->Measures.size()));
  } else if (cancelled.load()) {
    archive_file::appendDebugLogLine("Parse cancelled " +
                                     std::string(logContext) + ": " +
                                     pathText);
  } else {
    archive_file::appendDebugLogLine("Parse returned null " +
                                     std::string(logContext) + ": " +
                                     pathText);
  }
  return std::unique_ptr<bms_parser::Chart>(parsedChart);
}

inline std::unique_ptr<bms_parser::Chart>
parseChart(const std::filesystem::path &path,
           const std::optional<unsigned int> &randomSeed,
           const std::optional<std::string> &randomPrng,
           std::atomic_bool &cancelled, std::string_view logContext = "chart") {
  return parseChart(path, randomSeed, randomPrng, std::nullopt, cancelled,
                    logContext);
}

inline std::unique_ptr<bms_parser::Chart>
parseChart(const std::filesystem::path &path, std::atomic_bool &cancelled,
           std::string_view logContext = "chart") {
  return parseChart(path, std::nullopt, std::nullopt, std::nullopt, cancelled,
                    logContext);
}

inline std::unique_ptr<bms_parser::Chart>
parseChart(const bms_parser::ChartMeta &meta, std::atomic_bool &cancelled,
           std::string_view logContext = "chart") {
  return parseChart(meta.BmsPath, meta.RandomSeed, meta.RandomPrng,
                    randomValuesOrNull(meta.RandomValues), cancelled,
                    logContext);
}

inline std::unique_ptr<bms_parser::Chart>
parseChartForReplay(const std::filesystem::path &path, const JudgedPlaybackData &replay,
                    std::atomic_bool &cancelled) {
  return parseChart(path, replay.randomSeed, replay.randomPrng,
                    randomValuesOrNull(replay.randomValues), cancelled,
                    "replay");
}

inline std::unique_ptr<bms_parser::Chart>
prepareReplayChart(const std::filesystem::path &path, const JudgedPlaybackData &replay,
                   std::atomic_bool &cancelled) {
  auto chart = parseChartForReplay(path, replay, cancelled);
  if (chart == nullptr || cancelled || !applyReplayPlayOptions(*chart, replay)) {
    return nullptr;
  }
  applyEffectiveLongNoteModeToChart(*chart, replay.chartMeta.LnMode);
  return chart;
}

inline std::unique_ptr<bms_parser::Chart> prepareReplayChart(
    const std::filesystem::path &path,
    const replay::ReplayPlaybackData &playback,
    std::atomic_bool &cancelled) {
  const auto &setup = playback.setup;
  auto chart = parseChart(path, setup.randomSeed, setup.randomPrng,
                          randomValuesOrNull(setup.randomValues), cancelled,
                          "raw replay");
  if (chart == nullptr || cancelled ||
      !replay::storedChartIdentityMatches(
          setup.chartSha256, setup.chartMd5, chart->Meta.SHA256,
          chart->Meta.MD5) ||
      !applyReplayPlayOptions(*chart, playback)) {
    return nullptr;
  }
  applyEffectiveLongNoteModeToChart(*chart, setup.longNoteMode);
  return chart;
}

inline std::unique_ptr<bms_parser::Chart>
parseChartForRetry(const JudgedPlaybackData &retrySource,
                   const bms_parser::ChartMeta &fallbackMeta,
                   std::atomic_bool &cancelled, bool samePattern) {
  const bms_parser::ChartMeta &chartMeta = retrySource.chartMeta.BmsPath.empty()
                                               ? fallbackMeta
                                               : retrySource.chartMeta;
  std::optional<std::string> randomPrng;
  std::optional<unsigned int> randomSeed;
  std::optional<std::vector<int>> randomValues;
  if (samePattern) {
    randomPrng = retrySource.randomPrng.has_value() ? retrySource.randomPrng
                                                    : chartMeta.RandomPrng;
    randomSeed = retrySource.randomSeed.has_value() ? retrySource.randomSeed
                                                    : chartMeta.RandomSeed;
    randomValues = !retrySource.randomValues.empty()
                       ? randomValuesOrNull(retrySource.randomValues)
                       : randomValuesOrNull(chartMeta.RandomValues);
  }
  auto chart = parseChart(chartMeta.BmsPath, randomSeed, randomPrng,
                          randomValues, cancelled, "retry");
  if (chart != nullptr && !cancelled && chart->Meta.LnMode == 0 &&
      normalizeChartLongNoteModeValue(chartMeta.LnMode) > 0) {
    chart->Meta.LnMode = chartMeta.LnMode;
  }
  return chart;
}

} // namespace play_options
