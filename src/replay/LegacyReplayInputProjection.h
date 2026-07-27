#pragma once

#include "ReplayPlaybackData.h"

#include <map>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

namespace replay {

struct LegacyReplayInputProjection {
  std::vector<InputTransition> input;
  bool stockScratchDirectionBestEffort = false;
};

[[nodiscard]] inline std::optional<LogicalControl>
legacyReplayControlForPhysicalLane(int physicalLane, int keyMode) noexcept {
  const auto lane = [](int player, int logicalLane) {
    return LogicalControl{.kind = LogicalControlKind::Lane,
                          .player = player,
                          .lane = logicalLane};
  };
  const auto scratch = [](int player) {
    return LogicalControl{.kind = LogicalControlKind::ScratchClockwise,
                          .player = player,
                          .lane = -1};
  };
  if (physicalLane < 0) {
    return std::nullopt;
  }
  switch (keyMode) {
  case 5:
    if (physicalLane < 5) {
      return lane(1, physicalLane);
    }
    return physicalLane == 7 ? std::optional<LogicalControl>(scratch(1))
                             : std::nullopt;
  case 7:
    if (physicalLane < 7) {
      return lane(1, physicalLane);
    }
    return physicalLane == 7 ? std::optional<LogicalControl>(scratch(1))
                             : std::nullopt;
  case 9:
    return physicalLane < 9
               ? std::optional<LogicalControl>(lane(1, physicalLane))
               : std::nullopt;
  case 10:
    if (physicalLane < 5) {
      return lane(1, physicalLane);
    }
    if (physicalLane == 7) {
      return scratch(1);
    }
    if (physicalLane >= 8 && physicalLane < 13) {
      return lane(2, physicalLane - 8);
    }
    return physicalLane == 15 ? std::optional<LogicalControl>(scratch(2))
                              : std::nullopt;
  case 14:
    if (physicalLane < 7) {
      return lane(1, physicalLane);
    }
    if (physicalLane == 7) {
      return scratch(1);
    }
    if (physicalLane >= 8 && physicalLane < 15) {
      return lane(2, physicalLane - 8);
    }
    return physicalLane == 15 ? std::optional<LogicalControl>(scratch(2))
                              : std::nullopt;
  case 24:
    return physicalLane < 26
               ? std::optional<LogicalControl>(lane(1, physicalLane))
               : std::nullopt;
  case 48:
    if (physicalLane < 26) {
      return lane(1, physicalLane);
    }
    return physicalLane < 52
               ? std::optional<LogicalControl>(lane(2, physicalLane - 26))
               : std::nullopt;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] inline std::optional<LegacyReplayInputProjection>
projectLegacyReplayInput(std::span<const LegacyPlaybackEvent> events,
                         int keyMode, int *invalidPhysicalLane = nullptr) {
  LegacyReplayInputProjection projection;
  projection.input.reserve(events.size());
  std::map<std::tuple<int, int, int>, bool> states;
  for (const LegacyPlaybackEvent &event : events) {
    if (event.action != LegacyPlaybackAction::Press &&
        event.action != LegacyPlaybackAction::Release) {
      continue;
    }
    const auto control =
        legacyReplayControlForPhysicalLane(event.lane, keyMode);
    if (!control.has_value()) {
      if (invalidPhysicalLane != nullptr) {
        *invalidPhysicalLane = event.lane;
      }
      return std::nullopt;
    }
    if (event.songTimeMicros < kMinimumReplaySongTimeMicros) {
      continue;
    }
    projection.stockScratchDirectionBestEffort |=
        control->kind == LogicalControlKind::ScratchClockwise ||
        control->kind == LogicalControlKind::ScratchCounterClockwise;
    const auto key = std::tuple(static_cast<int>(control->kind),
                                control->player, control->lane);
    const bool pressed = event.action == LegacyPlaybackAction::Press;
    if (states[key] == pressed) {
      continue;
    }
    states[key] = pressed;
    projection.input.push_back({.songTimeMicros = event.songTimeMicros,
                                .control = *control,
                                .pressed = pressed});
  }
  return projection;
}

} // namespace replay
