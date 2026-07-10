#include "LogicalGameplayInputAdapter.h"

#include <SDL2/SDL_scancode.h>

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

InputProfile makeGameplayInputProfileWithEscapeFallback(
    const InputProfile &profile,
    std::span<const input::InputScope> activeScopes) {
  InputProfile result = profile;
  if (activeScopes.empty() ||
      hasActiveKeyboardActionBinding(result, activeScopes, SDL_SCANCODE_ESCAPE,
                                     input::LogicalActionKind::Pause)) {
    return result;
  }
  const auto scope = activeScopes.front();
  result.bindings.push_back(
      {.id = "compat-keyboard-escape-pause-p" + std::to_string(scope.player) +
             "-k" + std::to_string(scope.keyMode),
       .scope = scope,
       .action = {.kind = input::LogicalActionKind::Pause},
       .control = {.deviceId = "keyboard",
                   .deviceClass = input::DeviceClass::Keyboard,
                   .kind = input::ControlKind::Key,
                   .index = SDL_SCANCODE_ESCAPE,
                   .direction = input::ControlDirection::Any}});
  return result;
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
      bool oppositeReleasedInBatch = false;
      if (!transition.pressed) {
        const int lane = scratchLane(transition.scope);
        for (std::size_t candidateIndex = 0;
             candidateIndex < transitions.size(); ++candidateIndex) {
          const auto &candidate = transitions[candidateIndex];
          const bool candidateIsScratch =
              candidate.action.kind ==
                  input::LogicalActionKind::ScratchClockwise ||
              candidate.action.kind ==
                  input::LogicalActionKind::ScratchCounterClockwise;
          if (!candidateIsScratch || scratchLane(candidate.scope) != lane) {
            continue;
          }
          const bool opposite =
              (direction == ScratchDirection::Clockwise &&
               candidate.action.kind ==
                   input::LogicalActionKind::ScratchCounterClockwise) ||
              (direction == ScratchDirection::CounterClockwise &&
               candidate.action.kind ==
                   input::LogicalActionKind::ScratchClockwise);
          if (!opposite) {
            continue;
          }
          if (!candidate.pressed) {
            oppositeReleasedInBatch = true;
          } else if (candidateIndex > index) {
            reversing = true;
          }
        }
      }
      applyScratch(transition, direction, reversing, oppositeReleasedInBatch);
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
  for (const auto &[lane, state] : scratchLaneStates_) {
    if (state.activeDirection.has_value()) {
      effectiveHeldLanes.insert(lane);
    }
  }
  for (const int lane : effectiveHeldLanes) {
    control_.releaseLane(lane, 0.0, false);
  }
  heldLaneScopes_.clear();
  scratchLaneStates_.clear();
}

int LogicalGameplayInputAdapter::scratchLane(input::InputScope scope) {
  return scope.player == 2 ? kSecondPlayerScratchLane : kFirstPlayerScratchLane;
}

bool LogicalGameplayInputAdapter::isLaneHeld(int lane) const {
  const auto scratch = scratchLaneStates_.find(lane);
  return heldLaneScopes_.contains(lane) ||
         (scratch != scratchLaneStates_.end() &&
          scratch->second.activeDirection.has_value());
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
    bool reversing, bool oppositeReleasedInBatch) {
  const int lane = scratchLane(transition.scope);
  if (!transition.pressed) {
    const auto found = scratchLaneStates_.find(lane);
    if (found == scratchLaneStates_.end() ||
        found->second.heldDirections.erase(direction) == 0) {
      return;
    }
    auto &state = found->second;
    if (state.activeDirection != direction) {
      if (state.heldDirections.empty()) {
        scratchLaneStates_.erase(found);
      }
      return;
    }

    if (!state.heldDirections.empty()) {
      state.activeDirection = *state.heldDirections.begin();
      if (oppositeReleasedInBatch) {
        return;
      }
      control_.releaseLane(lane, 0.0, true);
      control_.pressLane(lane);
      return;
    }

    state.activeDirection.reset();
    const bool digitalLaneHeld = heldLaneScopes_.contains(lane);
    if (reversing || !digitalLaneHeld) {
      control_.releaseLane(lane, 0.0, reversing);
      if (reversing && digitalLaneHeld) {
        control_.pressLane(lane);
      }
    }
    scratchLaneStates_.erase(found);
    return;
  }

  const bool wasHeld = isLaneHeld(lane);
  auto &state = scratchLaneStates_[lane];
  state.heldDirections.insert(direction);
  if (state.activeDirection == direction) {
    return;
  }
  if (state.activeDirection.has_value()) {
    control_.releaseLane(lane, 0.0, true);
    control_.pressLane(lane);
    state.activeDirection = direction;
    return;
  }
  state.activeDirection = direction;
  if (!wasHeld) {
    control_.pressLane(lane);
  }
}

LogicalGameplayInputPipeline::LogicalGameplayInputPipeline(
    IRhythmControl &control, const InputProfile &profile,
    std::vector<input::InputScope> activeScopes,
    LogicalGameplayInputAdapter::CommandCallback commandCallback,
    LogicalGameplayRegistryPolicy registryPolicy)
    : adapter_(control, std::move(commandCallback)),
      resolver_(
          profile, std::move(activeScopes),
          {.onTransitions =
               [this](
                   std::span<const input::LogicalInputTransition> transitions) {
                 adapter_.apply(transitions);
               }}),
      registryPolicy_(registryPolicy) {}

bool LogicalGameplayInputPipeline::consumeRegistryEvent(
    const input::PhysicalInputEvent &event) {
  if (!registryPolicy_.acceptKeyboardFromRegistry &&
      event.control.deviceClass == input::DeviceClass::Keyboard) {
    return false;
  }
  resolver_.consume(event);
  return true;
}

bool LogicalGameplayInputPipeline::consumeDirectKeyboard(int scancode,
                                                         bool pressed) {
  if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_NUM_SCANCODES) {
    return false;
  }
  resolver_.consume({.control = {.deviceId = "keyboard",
                                 .deviceClass = input::DeviceClass::Keyboard,
                                 .kind = input::ControlKind::Key,
                                 .index = scancode,
                                 .direction = input::ControlDirection::Any},
                     .rawValue = pressed ? 1.0 : 0.0,
                     .normalizedValue = pressed ? 1.0F : 0.0F});
  return true;
}

void LogicalGameplayInputPipeline::disconnectDevice(std::string_view stableId) {
  resolver_.disconnectDevice(stableId);
}

void LogicalGameplayInputPipeline::reset() {
  resolver_.reset();
  adapter_.reset();
}
