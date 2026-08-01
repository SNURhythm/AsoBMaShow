#pragma once

#include <optional>

namespace replay {

[[nodiscard]] constexpr std::optional<int>
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

[[nodiscard]] constexpr std::optional<int>
applicationLongNoteMode(int stockMode) noexcept {
  if (stockMode < 0 || stockMode > 2) {
    return std::nullopt;
  }
  return stockMode + 1;
}

[[nodiscard]] constexpr bool
hasUndefinedLongNotesForReplay(int authoredLongNoteMode, int totalLongNotes,
                               int totalBackSpinNotes) noexcept {
  return authoredLongNoteMode == 0 &&
         (totalLongNotes > 0 || totalBackSpinNotes > 0);
}

} // namespace replay
