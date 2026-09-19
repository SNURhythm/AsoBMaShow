#pragma once

#include "PlayOptionUtils.h"
#include "replay/ReplayOption.h"

#include <algorithm>
#include <unordered_map>

inline std::optional<std::vector<int>> resultReplayLanePattern(
    const bms_parser::ChartMeta &meta, const std::optional<std::string> &option,
    const std::optional<long long> &seed, int player) {
  const auto optionIndex = option
                               ? replay::projectedBeatorajaReplayOptionIndex(*option)
                               : std::nullopt;
  if (!optionIndex || (*optionIndex != 2 && *optionIndex != 3 &&
                       *optionIndex != 8)) {
    return std::nullopt;
  }
  const auto laneOrder = play_options::laneOrderForPlayOption(
      meta, option, seed, player);
  if (!laneOrder) return std::nullopt;
  const auto destinations = meta.GetTotalLaneIndices();
  if (destinations.size() != laneOrder->size()) return std::nullopt;
  std::unordered_map<int, int> sourceByDestination;
  for (std::size_t index = 0; index < destinations.size(); ++index) {
    sourceByDestination.emplace(destinations[index], (*laneOrder)[index]);
  }
  const int keyCount = meta.KeyMode == 10 ? 5
                       : meta.KeyMode == 14 ? 7
                                            : meta.KeyMode;
  const auto keys = meta.GetKeyLaneIndices();
  const auto scratches = meta.GetScratchLaneIndices();
  const std::size_t keyOffset = player == 0 ? 0 : static_cast<std::size_t>(keyCount);
  if (keyCount <= 0 ||
      keyOffset + static_cast<std::size_t>(keyCount) > keys.size()) {
    return std::nullopt;
  }
  const int playerOffset = player == 1 ? keyCount : 0;
  std::vector<int> sourceLanes;
  sourceLanes.reserve(static_cast<std::size_t>(keyCount +
      (static_cast<std::size_t>(player) < scratches.size() ? 1 : 0)));
  for (int key = 0; key < keyCount; ++key) {
    sourceLanes.push_back(keys[keyOffset + static_cast<std::size_t>(key)]);
  }
  if (static_cast<std::size_t>(player) < scratches.size()) {
    sourceLanes.push_back(scratches[static_cast<std::size_t>(player)]);
  }
  std::vector<int> pattern;
  pattern.reserve(sourceLanes.size());
  const auto appendSourceOrdinal = [&](int sourceLane) {
    const auto source = std::ranges::find(sourceLanes, sourceLane);
    if (source == sourceLanes.end()) return false;
    pattern.push_back(static_cast<int>(source - sourceLanes.begin()) +
                      playerOffset);
    return true;
  };
  for (int key = 0; key < keyCount; ++key) {
    const auto found = sourceByDestination.find(
        keys[keyOffset + static_cast<std::size_t>(key)]);
    if (found == sourceByDestination.end()) return std::nullopt;
    if (!appendSourceOrdinal(found->second)) return std::nullopt;
  }
  if (static_cast<std::size_t>(player) < scratches.size()) {
    const auto scratch = sourceByDestination.find(
        scratches[static_cast<std::size_t>(player)]);
    if (scratch == sourceByDestination.end()) return std::nullopt;
    if (!appendSourceOrdinal(scratch->second)) return std::nullopt;
  }
  return pattern;
}
