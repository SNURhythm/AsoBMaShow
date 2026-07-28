#pragma once

#include "ReplayKeyMode.h"

#include <array>
#include <optional>
#include <string_view>

namespace replay {

inline constexpr std::array<std::string_view, 10> kBeatorajaReplayOptions{
    "NORMAL", "MIRROR",   "RANDOM",  "R-RANDOM",  "S-RANDOM",
    "SPIRAL", "H-RANDOM", "ALL-SCR", "RANDOM-EX", "S-RANDOM-EX",
};

[[nodiscard]] inline constexpr std::optional<int>
beatorajaReplayOptionIndex(std::string_view name) noexcept {
  for (std::size_t index = 0; index < kBeatorajaReplayOptions.size(); ++index) {
    if (kBeatorajaReplayOptions[index] == name) {
      return static_cast<int>(index);
    }
  }
  return std::nullopt;
}

[[nodiscard]] inline constexpr std::optional<std::string_view>
beatorajaReplayOptionName(int index) noexcept {
  if (index < 0 || index >= static_cast<int>(kBeatorajaReplayOptions.size())) {
    return std::nullopt;
  }
  return kBeatorajaReplayOptions[static_cast<std::size_t>(index)];
}

[[nodiscard]] inline constexpr std::optional<int>
projectedBeatorajaReplayOptionIndex(std::string_view name) noexcept {
  if (const auto stock = beatorajaReplayOptionIndex(name)) {
    return stock;
  }
  return name.starts_with("ASSIGN:") ? std::optional<int>{0} : std::nullopt;
}

[[nodiscard]] inline constexpr bool
validReplayPlayerOptionName(std::string_view name, int keyMode) noexcept {
  if (beatorajaReplayOptionIndex(name)) {
    return true;
  }
  constexpr std::string_view prefix = "ASSIGN:";
  if (!name.starts_with(prefix)) {
    return false;
  }
  const std::string_view symbols = manualAssignmentSymbols(keyMode);
  const std::string_view notation = name.substr(prefix.size());
  if (symbols.empty() || notation.size() != symbols.size()) {
    return false;
  }
  for (std::size_t index = 0; index < notation.size(); ++index) {
    if (symbols.find(notation[index]) == std::string_view::npos ||
        notation.find(notation[index]) != index) {
      return false;
    }
  }
  return true;
}

} // namespace replay
