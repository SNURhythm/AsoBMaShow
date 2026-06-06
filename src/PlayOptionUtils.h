#pragma once

#include "ReplayData.h"
#include "bms_parser.hpp"

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
#include <vector>

namespace play_options {

struct PlayOptionReplayInfo {
  std::optional<std::string> option;
  std::optional<long long> seed;
  std::optional<std::string> option2;
  std::optional<long long> seed2;
};

inline constexpr std::array<const char *, 10> kPlayOptions = {
    "NORMAL", "MIRROR",   "RANDOM",  "R-RANDOM",  "S-RANDOM",
    "SPIRAL", "H-RANDOM", "ALL-SCR", "RANDOM-EX", "S-RANDOM-EX"};

inline std::string normalizePlayOption(std::string option) {
  option.erase(option.begin(),
               std::find_if(option.begin(), option.end(), [](unsigned char c) {
                 return std::isspace(c) == 0;
               }));
  option.erase(
      std::find_if(option.rbegin(), option.rend(),
                   [](unsigned char c) { return std::isspace(c) == 0; })
          .base(),
      option.end());
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
  return "NORMAL";
}

inline std::string nextPlayOption(const std::string &option) {
  const std::string normalized = normalizePlayOption(option);
  for (size_t i = 0; i < kPlayOptions.size(); ++i) {
    if (normalized == kPlayOptions[i]) {
      return kPlayOptions[(i + 1) % kPlayOptions.size()];
    }
  }
  return kPlayOptions.front();
}

inline bool isNormalPlayOption(const std::string &option) {
  return normalizePlayOption(option) == "NORMAL";
}

inline std::string
formatPlayOptionLabel(const std::optional<std::string> &option,
                      const std::optional<long long> &seed = std::nullopt,
                      const std::optional<std::string> &option2 = std::nullopt,
                      const std::optional<long long> &seed2 = std::nullopt) {
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

inline bool
applyPlayOptionModifier(bms_parser::Chart &chart, const std::string &option,
                        std::optional<long long> seed, int player,
                        std::optional<std::string> &appliedOption,
                        std::optional<long long> &appliedSeed,
                        std::string_view logContext = "play option") {
  const std::string normalized = normalizePlayOption(option);
  if (normalized.empty() || normalized == "NORMAL") {
    return true;
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
  appliedSeed = modifier->GetSeed();
  return true;
}

inline bool applyReplayPlayOptions(bms_parser::Chart &chart,
                                   const ReplayData &replay) {
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

inline PlayOptionReplayInfo
applySelectedPlayOptions(bms_parser::Chart &chart, const std::string &option) {
  PlayOptionReplayInfo info;
  if (!applyPlayOptionModifier(chart, option, std::nullopt, 0, info.option,
                               info.seed)) {
    return {};
  }
  if (chart.Meta.IsDP &&
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

inline std::unique_ptr<bms_parser::Chart>
parseChart(const std::filesystem::path &path,
           const std::optional<unsigned int> &randomSeed,
           const std::optional<std::string> &randomPrng,
           const std::optional<std::vector<int>> &randomValues,
           std::atomic_bool &cancelled, std::string_view logContext = "chart") {
  if (path.empty()) {
    SDL_Log("Cannot parse %.*s: chart path is empty",
            static_cast<int>(logContext.size()), logContext.data());
    return nullptr;
  }

  bms_parser::Parser parser;
  if (randomPrng.has_value() && !parser.SetRandomPrng(*randomPrng)) {
    SDL_Log("Unsupported %.*s random PRNG: %s",
            static_cast<int>(logContext.size()), logContext.data(),
            randomPrng->c_str());
    return nullptr;
  }
  if (randomSeed.has_value()) {
    parser.SetRandomSeed(*randomSeed);
  }
  if (randomValues.has_value()) {
    parser.SetRandomValues(*randomValues);
  }

  bms_parser::Chart *parsedChart = nullptr;
  parser.Parse(path, &parsedChart, false, false, cancelled);
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
parseChartForReplay(const std::filesystem::path &path, const ReplayData &replay,
                    std::atomic_bool &cancelled) {
  return parseChart(path, replay.randomSeed, replay.randomPrng,
                    randomValuesOrNull(replay.randomValues), cancelled,
                    "replay");
}

inline std::unique_ptr<bms_parser::Chart>
prepareReplayChart(const std::filesystem::path &path, const ReplayData &replay,
                   std::atomic_bool &cancelled) {
  auto chart = parseChartForReplay(path, replay, cancelled);
  if (chart == nullptr || cancelled || !applyReplayPlayOptions(*chart, replay)) {
    return nullptr;
  }
  return chart;
}

inline std::unique_ptr<bms_parser::Chart>
parseChartForRetry(const ReplayData &retrySource,
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
  return parseChart(chartMeta.BmsPath, randomSeed, randomPrng, randomValues,
                    cancelled, "retry");
}

} // namespace play_options
