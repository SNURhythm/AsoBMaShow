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
  const auto layout = replay::replayKeyModeLayout(meta.KeyMode);
  if (!layout || player < 0 || player >= layout->players) return std::nullopt;
  // Pop'n 5K shares the 9K replay identity. Keep seed reproduction instead
  // of exporting a five-entry pattern into the nine-entry stock namespace.
  if (meta.IsPopnFiveKey()) return std::nullopt;
  const auto laneOrder = play_options::laneOrderForPlayOption(
      meta, option, seed, player);
  if (!laneOrder) return std::nullopt;
  const auto destinations = meta.GetTotalLaneIndices();
  if (destinations.size() != laneOrder->size()) return std::nullopt;
  std::unordered_map<int, int> sourceByDestination;
  for (std::size_t index = 0; index < destinations.size(); ++index) {
    sourceByDestination.emplace(destinations[index], (*laneOrder)[index]);
  }
  const auto keys = meta.GetKeyLaneIndices();
  const auto scratches = meta.GetScratchLaneIndices();
  const std::size_t keyCount = keys.size() / layout->players;
  const std::size_t scratchCount = scratches.size() / layout->players;
  if (keyCount == 0 || keys.size() % layout->players != 0 ||
      scratches.size() % layout->players != 0) return std::nullopt;
  const std::size_t keyOffset = player * keyCount;
  const std::size_t scratchOffset = player * scratchCount;
  // Result skins use global source ordinals: beat modes retain their legacy
  // key-count offset, while keyboard decks each occupy all 26 note lanes.
  const int playerOffset = player == 0 ? 0
      : meta.KeyMode == 48 ? 26 : static_cast<int>(keyCount);
  std::vector<int> sourceLanes;
  sourceLanes.reserve(keyCount + scratchCount);
  for (std::size_t key = 0; key < keyCount; ++key) {
    sourceLanes.push_back(keys[keyOffset + key]);
  }
  for (std::size_t scratch = 0; scratch < scratchCount; ++scratch) {
    sourceLanes.push_back(scratches[scratchOffset + scratch]);
  }
  std::vector<int> pattern;
  pattern.reserve(sourceLanes.size());
  for (const int destination : sourceLanes) {
    const auto found = sourceByDestination.find(destination);
    if (found == sourceByDestination.end()) return std::nullopt;
    const auto source = std::ranges::find(sourceLanes, found->second);
    if (source == sourceLanes.end()) return std::nullopt;
    pattern.push_back(static_cast<int>(source - sourceLanes.begin()) +
                      playerOffset);
  }
  return pattern;
}
