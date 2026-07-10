#include "LogicalGameplayInputAdapter.h"

#include <algorithm>
#include <utility>

namespace {
constexpr int kFirstPlayerScratchLane = 7;
constexpr int kSecondPlayerScratchLane = 15;
constexpr int kMinimumGameplayLane = 0;
constexpr int kMaximumGameplayLane = 15;

bool isValidGameplayLane(int lane) {
  return lane >= kMinimumGameplayLane && lane <= kMaximumGameplayLane;
}
} // namespace

std::vector<input::InputScope> makeGameplayInputScopes(int keyMode) {
  std::vector<input::InputScope> scopes{{.player = 1, .keyMode = keyMode}};
  if (keyMode == 10 || keyMode == 14) {
    scopes.push_back({.player = 2, .keyMode = keyMode});
  }
  return scopes;
}

bool hasActiveKeyboardActionBinding(
    const InputProfile &profile,
    std::span<const input::InputScope> activeScopes, int scancode,
    input::LogicalActionKind actionKind) {
  return std::ranges::any_of(profile.bindings, [&](const auto &binding) {
    return std::ranges::find(activeScopes, binding.scope) !=
               activeScopes.end() &&
           binding.action.kind == actionKind &&
           binding.control.deviceId == "keyboard" &&
           binding.control.deviceClass == input::DeviceClass::Keyboard &&
           binding.control.kind == input::ControlKind::Key &&
           binding.control.index == scancode &&
           binding.control.direction == input::ControlDirection::Any;
  });
}

LogicalGameplayInputAdapter::LogicalGameplayInputAdapter(
    IRhythmControl &control, CommandCallback commandCallback)
    : control_(control), commandCallback_(std::move(commandCallback)) {}

void LogicalGameplayInputAdapter::apply(
    std::span<const input::LogicalInputTransition> transitions) {
  for (std::size_t index = 0; index < transitions.size(); ++index) {
    const auto &transition = transitions[index];
    switch (transition.action.kind) {
    case input::LogicalActionKind::Lane:
      applyLane(transition);
      break;
    case input::LogicalActionKind::ScratchClockwise:
    case input::LogicalActionKind::ScratchCounterClockwise: {
      const ScratchDirection direction =
          transition.action.kind == input::LogicalActionKind::ScratchClockwise
              ? ScratchDirection::Clockwise
              : ScratchDirection::CounterClockwise;
      bool reversing = false;
      if (!transition.pressed) {
        const int lane = scratchLane(transition.scope);
        for (std::size_t next = index + 1; next < transitions.size(); ++next) {
          const auto &candidate = transitions[next];
          const bool candidateIsScratch =
              candidate.action.kind ==
                  input::LogicalActionKind::ScratchClockwise ||
              candidate.action.kind ==
                  input::LogicalActionKind::ScratchCounterClockwise;
          if (!candidateIsScratch || !candidate.pressed ||
              scratchLane(candidate.scope) != lane) {
            continue;
          }
          const bool opposite =
              (direction == ScratchDirection::Clockwise &&
               candidate.action.kind ==
                   input::LogicalActionKind::ScratchCounterClockwise) ||
              (direction == ScratchDirection::CounterClockwise &&
               candidate.action.kind ==
                   input::LogicalActionKind::ScratchClockwise);
          if (opposite) {
            reversing = true;
            break;
          }
        }
      }
      applyScratch(transition, direction, reversing);
      break;
    }
    case input::LogicalActionKind::Start:
    case input::LogicalActionKind::Select:
    case input::LogicalActionKind::Pause:
    case input::LogicalActionKind::Retry:
    case input::LogicalActionKind::LaneCoverIncrease:
    case input::LogicalActionKind::LaneCoverDecrease:
      if (commandCallback_) {
        commandCallback_(transition);
      }
      break;
    }
  }
}

void LogicalGameplayInputAdapter::reset() {
  std::set<int> effectiveHeldLanes;
  for (const auto &[lane, scopes] : heldLaneScopes_) {
    (void)scopes;
    effectiveHeldLanes.insert(lane);
  }
  for (const auto &[lane, direction] : heldScratchDirections_) {
    (void)direction;
    effectiveHeldLanes.insert(lane);
  }
  for (const int lane : effectiveHeldLanes) {
    control_.releaseLane(lane, 0.0, false);
  }
  heldLaneScopes_.clear();
  heldScratchDirections_.clear();
}

int LogicalGameplayInputAdapter::scratchLane(input::InputScope scope) {
  return scope.player == 2 ? kSecondPlayerScratchLane : kFirstPlayerScratchLane;
}

bool LogicalGameplayInputAdapter::isLaneHeld(int lane) const {
  return heldLaneScopes_.contains(lane) ||
         heldScratchDirections_.contains(lane);
}

void LogicalGameplayInputAdapter::applyLane(
    const input::LogicalInputTransition &transition) {
  const int lane = transition.action.lane;
  if (!isValidGameplayLane(lane)) {
    return;
  }
  const bool wasHeld = isLaneHeld(lane);
  if (transition.pressed) {
    const bool inserted = heldLaneScopes_[lane].insert(transition.scope).second;
    if (inserted && !wasHeld) {
      control_.pressLane(lane);
    }
    return;
  }
  const auto held = heldLaneScopes_.find(lane);
  if (held == heldLaneScopes_.end() ||
      held->second.erase(transition.scope) == 0) {
    return;
  }
  if (held->second.empty()) {
    heldLaneScopes_.erase(held);
  }
  if (!isLaneHeld(lane)) {
    control_.releaseLane(lane, 0.0, false);
  }
}

void LogicalGameplayInputAdapter::applyScratch(
    const input::LogicalInputTransition &transition, ScratchDirection direction,
    bool reversing) {
  const int lane = scratchLane(transition.scope);
  const auto held = heldScratchDirections_.find(lane);
  if (!transition.pressed) {
    if (held != heldScratchDirections_.end() && held->second == direction) {
      heldScratchDirections_.erase(held);
      if (reversing || !isLaneHeld(lane)) {
        control_.releaseLane(lane, 0.0, reversing);
        if (reversing && isLaneHeld(lane)) {
          control_.pressLane(lane);
        }
      }
    }
    return;
  }

  if (held != heldScratchDirections_.end()) {
    if (held->second == direction) {
      return;
    }
    control_.releaseLane(lane, 0.0, true);
    control_.pressLane(lane);
    heldScratchDirections_.insert_or_assign(lane, direction);
    return;
  }
  const bool wasHeld = isLaneHeld(lane);
  heldScratchDirections_.insert_or_assign(lane, direction);
  if (!wasHeld) {
    control_.pressLane(lane);
  }
}
