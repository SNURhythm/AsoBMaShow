#pragma once

#include "../ResultContracts.h"

#include <array>
#include <optional>
#include <string_view>

namespace replay {

struct ReplayKeyModeLayout {
  int keyMode = 0;
  int players = 0;
  int logicalLanesPerPlayer = 0;
  int stockShuffleWidth = 0;
  bool hasDirectionalScratch = false;
  bool supportsDoublePlayFlip = false;
  std::string_view manualAssignmentSymbols;

  bool operator==(const ReplayKeyModeLayout &) const = default;
};

inline constexpr std::array<ReplayKeyModeLayout, 7> kReplayKeyModeLayouts{{
    {5, 1, 5, 6, true, false, "S12345"},
    {7, 1, 7, 8, true, false, "S1234567"},
    {9, 1, 9, 9, false, false, ""},
    {10, 2, 5, 6, true, true, "L123456789AR"},
    {14, 2, 7, 8, true, true, "L123456789ABCDER"},
    {24, 1, 26, 26, false, false, ""},
    {48, 2, 26, 26, false, false, ""},
}};

static_assert([] {
  for (std::size_t index = 0; index < kReplayKeyModeLayouts.size(); ++index) {
    const auto &layout = kReplayKeyModeLayouts[index];
    if (!result_contract::isSupportedKeyMode(layout.keyMode)) {
      return false;
    }
    for (std::size_t candidate = index + 1;
         candidate < kReplayKeyModeLayouts.size(); ++candidate) {
      if (layout.keyMode == kReplayKeyModeLayouts[candidate].keyMode) {
        return false;
      }
    }
  }
  return true;
}());

[[nodiscard]] inline constexpr std::optional<ReplayKeyModeLayout>
replayKeyModeLayout(int keyMode) noexcept {
  for (const auto &layout : kReplayKeyModeLayouts) {
    if (layout.keyMode == keyMode) {
      return layout;
    }
  }
  return std::nullopt;
}

[[nodiscard]] inline constexpr std::string_view
manualAssignmentSymbols(int keyMode) noexcept {
  const auto layout = replayKeyModeLayout(keyMode);
  return layout ? layout->manualAssignmentSymbols : std::string_view{};
}

} // namespace replay
