#pragma once

#include "ReplayPlaybackData.h"

#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <string>

namespace replay {

[[nodiscard]] inline bool isScratchControlKind(
    LogicalControlKind kind) noexcept {
  return kind == LogicalControlKind::ScratchClockwise ||
         kind == LogicalControlKind::ScratchCounterClockwise;
}

[[nodiscard]] inline bool validateReplayOnlyScratchHandoffs(
    std::span<const InputTransition> input,
    std::string &diagnostic) noexcept {
  struct PendingHandoff {
    std::int64_t songTimeMicros = 0;
    LogicalControlKind released = LogicalControlKind::ScratchClockwise;
    bool active = false;
  };
  std::array<std::optional<LogicalControlKind>, 3> activeScratchDirections{};
  std::array<PendingHandoff, 3> pendingHandoffs{};
  const auto reject = [&diagnostic]() {
    diagnostic =
        "Replay-only input must be a same-timestamp opposite scratch "
        "ownership handoff on an already-held player scratch";
    return false;
  };

  for (std::size_t index = 0; index < input.size(); ++index) {
    const auto &transition = input[index];
    for (std::size_t playerIndex = 1; playerIndex < pendingHandoffs.size();
         ++playerIndex) {
      if (pendingHandoffs[playerIndex].active &&
          transition.songTimeMicros >
              pendingHandoffs[playerIndex].songTimeMicros) {
        return reject();
      }
    }
    const bool scratch = isScratchControlKind(transition.control.kind);
    const int player = transition.control.player;

    if (transition.replayOnly) {
      if (!scratch || player < 1 || player > 2) {
        return reject();
      }
      auto &active = activeScratchDirections[static_cast<std::size_t>(player)];
      auto &pending = pendingHandoffs[static_cast<std::size_t>(player)];
      if (!transition.pressed) {
        if (pending.active || !active || *active != transition.control.kind) {
          return reject();
        }
        pending = {.songTimeMicros = transition.songTimeMicros,
                   .released = transition.control.kind,
                   .active = true};
        continue;
      }
      if (!pending.active ||
          pending.songTimeMicros != transition.songTimeMicros ||
          pending.released == transition.control.kind) {
        return reject();
      }
      active = transition.control.kind;
      pending = {};
      continue;
    }

    if (!scratch || player < 1 || player > 2) {
      continue;
    }
    if (pendingHandoffs[static_cast<std::size_t>(player)].active) {
      return reject();
    }
    auto &active = activeScratchDirections[static_cast<std::size_t>(player)];
    if (transition.pressed) {
      active = transition.control.kind;
    } else if (active && *active == transition.control.kind) {
      active.reset();
    }
  }
  if (std::ranges::any_of(pendingHandoffs,
                          [](const auto &pending) { return pending.active; })) {
    return reject();
  }
  return true;
}

} // namespace replay
