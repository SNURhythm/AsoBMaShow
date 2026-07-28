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

bool isScratchControl(const replay::LogicalControl &control) {
  return control.kind == replay::LogicalControlKind::ScratchClockwise ||
         control.kind == replay::LogicalControlKind::ScratchCounterClockwise;
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
    IRhythmControl &control, CommandCallback commandCallback,
    AppliedTransitionCallback appliedTransitionCallback)
    : control_(control), commandCallback_(std::move(commandCallback)),
      appliedTransitionCallback_(std::move(appliedTransitionCallback)) {}

void LogicalGameplayInputAdapter::apply(
    std::span<const input::LogicalInputTransition> transitions) {
  applyOwned(transitions, OwnerKind::Logical);
}

bms_parser::Note *LogicalGameplayInputAdapter::applyTouch(
    const input::LogicalInputTransition &transition) {
  applyOwned(std::span(&transition, 1), OwnerKind::Touch);
  return latestPressedNote_;
}

void LogicalGameplayInputAdapter::applyOwned(
    std::span<const input::LogicalInputTransition> transitions,
    OwnerKind ownerKind) {
  latestPressedNote_ = nullptr;
  for (std::size_t index = 0; index < transitions.size(); ++index) {
    const auto &transition = transitions[index];
    switch (transition.action.kind) {
    case input::LogicalActionKind::Lane:
      applyLane(transition, ownerKind);
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
      applyScratch(transition, direction, ownerKind, reversing,
                   oppositeReleasedInBatch);
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
      if (transition.action.kind == input::LogicalActionKind::Start ||
          transition.action.kind == input::LogicalActionKind::Select) {
        notifyCommandApplied(transition);
      }
      break;
    }
  }
}

void LogicalGameplayInputAdapter::reset() {
  std::set<int> effectiveHeldLanes;
  for (const auto &[lane, owners] : heldLaneOwners_) {
    (void)owners;
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
  heldLaneOwners_.clear();
  scratchLaneStates_.clear();
  recordedScratchControls_.clear();
  pendingPhysicalEdges_.clear();
  latestPressedNote_ = nullptr;
}

int LogicalGameplayInputAdapter::scratchLane(input::InputScope scope) {
  return scope.player == 2 ? kSecondPlayerScratchLane : kFirstPlayerScratchLane;
}

bool LogicalGameplayInputAdapter::isLaneHeld(int lane) const {
  const auto scratch = scratchLaneStates_.find(lane);
  return heldLaneOwners_.contains(lane) ||
         (scratch != scratchLaneStates_.end() &&
          scratch->second.activeDirection.has_value());
}

bms_parser::Note *LogicalGameplayInputAdapter::pressPhysicalLane(int lane) {
  ++pendingPhysicalEdges_[lane];
  latestPressedNote_ = control_.pressLane(lane);
  return latestPressedNote_;
}

void LogicalGameplayInputAdapter::releasePhysicalLane(int lane,
                                                      bool backSpin) {
  ++pendingPhysicalEdges_[lane];
  control_.releaseLane(lane, 0.0, backSpin);
}

void LogicalGameplayInputAdapter::applyLane(
    const input::LogicalInputTransition &transition, OwnerKind ownerKind) {
  const int lane = transition.action.lane;
  if (!isValidGameplayLane(lane)) {
    return;
  }
  const bool wasHeld = isLaneHeld(lane);
  const replay::LogicalControl logicalControl = replayLaneControl(transition);
  const bool digitalScratch = isScratchControl(logicalControl);
  const LaneOwner owner{.scope = transition.scope, .kind = ownerKind};
  if (transition.pressed) {
    const bool inserted = heldLaneOwners_[lane].insert(owner).second;
    if (inserted) {
      if (!wasHeld) {
        pressPhysicalLane(lane);
      }
      if (digitalScratch) {
        synchronizeScratchReplayControl(transition, lane);
      } else if (!wasHeld) {
        notifyApplied(transition, logicalControl, true);
      }
    }
    return;
  }
  const auto held = heldLaneOwners_.find(lane);
  if (held == heldLaneOwners_.end() || held->second.erase(owner) == 0) {
    return;
  }
  if (held->second.empty()) {
    heldLaneOwners_.erase(held);
  }
  const bool releasedPhysicalLane = !isLaneHeld(lane);
  if (releasedPhysicalLane) {
    releasePhysicalLane(lane, false);
    if (!digitalScratch) {
      notifyApplied(transition, logicalControl, false);
    }
  }
  if (digitalScratch) {
    synchronizeScratchReplayControl(transition, lane);
  }
}

void LogicalGameplayInputAdapter::applyScratch(
    const input::LogicalInputTransition &transition, ScratchDirection direction,
    OwnerKind ownerKind, bool reversing, bool oppositeReleasedInBatch) {
  const int lane = scratchLane(transition.scope);
  const ScratchOwner owner{.direction = direction,
                           .scope = transition.scope,
                           .kind = ownerKind};
  if (!transition.pressed) {
    const auto found = scratchLaneStates_.find(lane);
    if (found == scratchLaneStates_.end() ||
        found->second.heldOwners.erase(owner) == 0) {
      return;
    }
    auto &state = found->second;
    if (state.activeDirection != direction) {
      if (state.heldOwners.empty()) {
        scratchLaneStates_.erase(found);
      }
      return;
    }

    const bool activeDirectionStillHeld = std::ranges::any_of(
        state.heldOwners, [direction](const ScratchOwner &held) {
          return held.direction == direction;
        });
    if (activeDirectionStillHeld) {
      return;
    }

    if (!state.heldOwners.empty()) {
      state.activeDirection = state.heldOwners.begin()->direction;
      if (oppositeReleasedInBatch) {
        return;
      }
      releasePhysicalLane(lane, true);
      pressPhysicalLane(lane);
      synchronizeScratchReplayControl(transition, lane);
      return;
    }

    state.activeDirection.reset();
    const bool digitalLaneHeld = heldLaneOwners_.contains(lane);
    if (reversing || !digitalLaneHeld) {
      releasePhysicalLane(lane, reversing);
      if (reversing && digitalLaneHeld) {
        pressPhysicalLane(lane);
      }
    }
    scratchLaneStates_.erase(found);
    synchronizeScratchReplayControl(transition, lane);
    return;
  }

  const bool wasHeld = isLaneHeld(lane);
  auto &state = scratchLaneStates_[lane];
  if (!state.heldOwners.insert(owner).second) {
    return;
  }
  if (state.activeDirection == direction) {
    return;
  }
  if (state.activeDirection.has_value()) {
    releasePhysicalLane(lane, true);
    pressPhysicalLane(lane);
    state.activeDirection = direction;
    synchronizeScratchReplayControl(transition, lane);
    return;
  }
  state.activeDirection = direction;
  if (!wasHeld) {
    pressPhysicalLane(lane);
  }
  synchronizeScratchReplayControl(transition, lane);
}

replay::LogicalControl LogicalGameplayInputAdapter::replayLaneControl(
    const input::LogicalInputTransition &transition) {
  const bool digitalScratch =
      (transition.scope.keyMode == 5 || transition.scope.keyMode == 7 ||
       transition.scope.keyMode == 10 || transition.scope.keyMode == 14) &&
      transition.action.lane == scratchLane(transition.scope);
  const auto control = replay::logicalControlForChartLane(
      transition.scope.keyMode, transition.action.lane, digitalScratch);
  return control.value_or(replay::LogicalControl{
      .kind = replay::LogicalControlKind::Lane,
      .player = transition.scope.player,
      .lane = -1});
}

std::optional<replay::LogicalControl>
LogicalGameplayInputAdapter::effectiveScratchReplayControl(int lane) const {
  const int player = lane == kSecondPlayerScratchLane ? 2 : 1;
  const auto scratch = scratchLaneStates_.find(lane);
  if (scratch != scratchLaneStates_.end() &&
      scratch->second.activeDirection.has_value()) {
    return replay::LogicalControl{
        .kind = *scratch->second.activeDirection == ScratchDirection::Clockwise
                    ? replay::LogicalControlKind::ScratchClockwise
                    : replay::LogicalControlKind::ScratchCounterClockwise,
        .player = player,
        .lane = -1};
  }
  if (heldLaneOwners_.contains(lane)) {
    return replay::LogicalControl{
        .kind = replay::LogicalControlKind::ScratchClockwise,
        .player = player,
        .lane = -1};
  }
  return std::nullopt;
}

void LogicalGameplayInputAdapter::synchronizeScratchReplayControl(
    const input::LogicalInputTransition &transition, int lane) {
  const auto recorded = recordedScratchControls_.find(lane);
  const std::optional<replay::LogicalControl> previous =
      recorded == recordedScratchControls_.end()
          ? std::nullopt
          : std::optional<replay::LogicalControl>(recorded->second);
  const auto next = effectiveScratchReplayControl(lane);
  if (previous == next) {
    return;
  }
  if (previous.has_value()) {
    notifyApplied(transition, *previous, false);
  }
  if (next.has_value()) {
    notifyApplied(transition, *next, true);
    recordedScratchControls_[lane] = *next;
  } else {
    recordedScratchControls_.erase(lane);
  }
}

void LogicalGameplayInputAdapter::notifyApplied(
    const input::LogicalInputTransition &source,
    replay::LogicalControl control, bool pressed) {
  const int lane = isScratchControl(control) ? scratchLane(source.scope)
                                             : source.action.lane;
  const auto pending = pendingPhysicalEdges_.find(lane);
  const bool replayOnly = pending == pendingPhysicalEdges_.end();
  if (!replayOnly && --pending->second == 0) {
    pendingPhysicalEdges_.erase(pending);
  }
  if (appliedTransitionCallback_) {
    appliedTransitionCallback_({.source = source,
                                .control = control,
                                .pressed = pressed,
                                .replayOnly = replayOnly});
  }
}

void LogicalGameplayInputAdapter::notifyCommandApplied(
    const input::LogicalInputTransition &transition) {
  if (!appliedTransitionCallback_) {
    return;
  }
  const auto kind = transition.action.kind == input::LogicalActionKind::Start
                        ? replay::LogicalControlKind::Start
                        : replay::LogicalControlKind::Select;
  appliedTransitionCallback_({.source = transition,
                              .control = {.kind = kind,
                                          .player = transition.scope.player,
                                          .lane = -1},
                              .pressed = transition.pressed,
                              .replayOnly = false});
}

LogicalGameplayInputPipeline::LogicalGameplayInputPipeline(
    IRhythmControl &control, const InputProfile &profile,
    std::vector<input::InputScope> activeScopes,
    LogicalGameplayInputAdapter::CommandCallback commandCallback,
    LogicalGameplayRegistryPolicy registryPolicy,
    LogicalGameplayInputAdapter::AppliedTransitionCallback
        appliedTransitionCallback)
    : adapter_(control, std::move(commandCallback),
               std::move(appliedTransitionCallback)),
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

bms_parser::Note *LogicalGameplayInputPipeline::consumeTouchTransition(
    const input::LogicalInputTransition &transition) {
  return adapter_.applyTouch(transition);
}

void LogicalGameplayInputPipeline::disconnectDevice(std::string_view stableId) {
  resolver_.disconnectDevice(stableId);
}

void LogicalGameplayInputPipeline::reset() {
  resolver_.reset();
  adapter_.reset();
}
