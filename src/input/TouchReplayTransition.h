#pragma once

#include "LogicalGameplayInputAdapter.h"

#include <array>
#include <cstdint>
#include <optional>

namespace touch_replay {

[[nodiscard]] inline LogicalGameplayInputAdapter::AppliedTransition
transition(int keyMode, int physicalLane, bool pressed,
           std::optional<int> scratchDirection,
           std::int64_t steadyTimestampMicros) {
  const bool playerTwo =
      ((keyMode == 10 || keyMode == 14) && physicalLane >= 8) ||
      (keyMode == 48 && physicalLane >= 26);
  const int player = playerTwo ? 2 : 1;
  const int playerTwoOffset = keyMode == 48 ? 26 : 8;
  const int localLane =
      playerTwo ? physicalLane - playerTwoOffset : physicalLane;
  const auto controlKind =
      scratchDirection.has_value()
          ? (*scratchDirection > 0
                 ? replay::LogicalControlKind::ScratchClockwise
                 : replay::LogicalControlKind::ScratchCounterClockwise)
          : replay::LogicalControlKind::Lane;
  auto actionKind = input::LogicalActionKind::Lane;
  if (scratchDirection.has_value()) {
    actionKind = *scratchDirection > 0
                     ? input::LogicalActionKind::ScratchClockwise
                     : input::LogicalActionKind::ScratchCounterClockwise;
  }
  return {
      .source = {.scope = {.player = player, .keyMode = keyMode},
                 .action = {.kind = actionKind, .lane = physicalLane},
                 .pressed = pressed,
                 .value = pressed ? 1.0F : 0.0F,
                 .steadyTimestampMicros = steadyTimestampMicros},
      .control = {.kind = controlKind,
                  .player = player,
                  .lane = scratchDirection.has_value() ? -1 : localLane},
      .pressed = pressed,
  };
}

[[nodiscard]] inline std::array<
    LogicalGameplayInputAdapter::AppliedTransition, 2>
scratchReversal(int keyMode, int physicalLane, int previousDirection,
                int nextDirection, std::int64_t steadyTimestampMicros) {
  return {
      transition(keyMode, physicalLane, false, previousDirection,
                 steadyTimestampMicros),
      transition(keyMode, physicalLane, true, nextDirection,
                 steadyTimestampMicros),
  };
}

} // namespace touch_replay
