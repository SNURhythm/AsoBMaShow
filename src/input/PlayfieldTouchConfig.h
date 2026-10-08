#pragma once

#include <compare>

namespace input {

enum class SideTapMode : unsigned char { EdgeLane, Scratch, Ignore };

struct PlayfieldTouchConfig {
  bool tapToScratch = false;
  SideTapMode sideTapMode = SideTapMode::EdgeLane;

  static constexpr bool supportsTapToScratch(int keyMode) {
    return keyMode == 5 || keyMode == 7 || keyMode == 10 || keyMode == 14;
  }

  void sanitize(int keyMode) {
    if (!supportsTapToScratch(keyMode)) {
      tapToScratch = false;
    }
    if (sideTapMode != SideTapMode::EdgeLane &&
        sideTapMode != SideTapMode::Scratch &&
        sideTapMode != SideTapMode::Ignore) {
      sideTapMode = SideTapMode::EdgeLane;
    }
    if (!tapToScratch && sideTapMode == SideTapMode::Scratch) {
      sideTapMode = SideTapMode::EdgeLane;
    }
  }

  auto operator<=>(const PlayfieldTouchConfig &) const = default;
};

} // namespace input
