#pragma once

#include <optional>

namespace replay {

[[nodiscard]] inline std::optional<int>
stockLongNoteMode(int applicationMode) noexcept {
  switch (applicationMode) {
  case 0:
  case 1:
    return 0;
  case 2:
    return 1;
  case 3:
    return 2;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] inline std::optional<int>
applicationLongNoteMode(int stockMode) noexcept {
  if (stockMode < 0 || stockMode > 2) {
    return std::nullopt;
  }
  return stockMode + 1;
}

} // namespace replay
