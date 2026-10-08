#include "LogicalGameplayInputAdapter.h"
#include "InputTimestamp.h"
#include "ChartLaneBinding.h"

#include <SDL2/SDL_timer.h>
#include <chrono>
#include <limits>

#include <SDL2/SDL_scancode.h>

#include <algorithm>
#include <utility>

namespace {
constexpr int kFirstPlayerScratchLane = 7;
constexpr int kSecondPlayerScratchLane = 15;

bool isValidGameplayLane(int lane) {
  return lane >= 0;
}

} // namespace

std::vector<input::InputScope> makeGameplayInputScopes(int keyMode) {
  std::vector<input::InputScope> scopes{{.player = 1, .keyMode = keyMode}};
  if (keyMode == 10 || keyMode == 14) {
    scopes.push_back({.player = 2, .keyMode = keyMode});
  }
  return scopes;
}

std::optional<replay::LogicalControl>
scratchCommandControl(const input::LogicalInputTransition &transition) {
  if (!input_profile::usesCommandOnlyScratch(transition.scope.keyMode)) {
    return std::nullopt;
  }
  switch (transition.action.kind) {
  case input::LogicalActionKind::ScratchClockwise:
    return replay::LogicalControl{.kind = replay::LogicalControlKind::ScratchClockwise,
                                  .player = transition.scope.player};
  case input::LogicalActionKind::ScratchCounterClockwise:
    return replay::LogicalControl{.kind = replay::LogicalControlKind::ScratchCounterClockwise,
                                  .player = transition.scope.player};
  default:
    return std::nullopt;
  }
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
  if (activeScopes.empty()) return result;
  const auto scope = activeScopes.front();
  if (!hasActiveKeyboardActionBinding(result, activeScopes, SDL_SCANCODE_ESCAPE,
                                      input::LogicalActionKind::Pause)) {
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
  }
  // Keyboard defaults are transient and yield to custom actions or occupied
  // keys in either player's active scope. Never rewrite the saved profile.
  for (const auto [key, action] : {
           std::pair{SDL_SCANCODE_Q, input::LogicalActionKind::Start},
           std::pair{SDL_SCANCODE_W, input::LogicalActionKind::Select},
           std::pair{SDL_SCANCODE_UP, input::LogicalActionKind::LaneCoverDecrease},
           std::pair{SDL_SCANCODE_DOWN, input::LogicalActionKind::LaneCoverIncrease},
           std::pair{SDL_SCANCODE_LSHIFT, input::LogicalActionKind::ScratchCounterClockwise},
           std::pair{SDL_SCANCODE_RSHIFT, input::LogicalActionKind::ScratchClockwise}}) {
    // Scratchless profiles still need scratch directions for Start/Select
    // commands, including profiles saved before these actions were exposed.
    const bool scratch = action == input::LogicalActionKind::ScratchClockwise ||
                         action == input::LogicalActionKind::ScratchCounterClockwise;
    if (scratch && scope.keyMode != -5 && scope.keyMode != -7 &&
        !input_profile::usesCommandOnlyScratch(scope.keyMode)) continue;
    const bool occupied = std::ranges::any_of(result.bindings, [&](const auto &binding) {
      return std::ranges::find(activeScopes, binding.scope) != activeScopes.end() &&
             (binding.action.kind == action ||
              (binding.control.deviceClass == input::DeviceClass::Keyboard &&
               binding.control.kind == input::ControlKind::Key && binding.control.index == key));
    });
    if (occupied) continue;
    result.bindings.push_back({
        .id = "compat-keyboard-cover-" + std::to_string(key),
        .scope = scope, .action = {.kind = action},
        .control = {.deviceId = "keyboard", .deviceClass = input::DeviceClass::Keyboard,
                    .kind = input::ControlKind::Key, .index = key,
                    .direction = input::ControlDirection::Any}});
  }
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
    auto transition = transitions[index];
    const auto receipt = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (transition.timestampMicros == 0) {
      transition.timestampMicros = receipt;
    } else if (transition.timestampDomain == input::InputTimestampDomain::SdlMilliseconds) {
      transition.timestampMicros = input::rebaseWrappingTimestampMillis(
          static_cast<std::uint32_t>(transition.timestampMicros / 1000),
          SDL_GetTicks(), receipt);
    }
    transition.timestampMicros = std::min<std::uint64_t>(
        transition.timestampMicros, std::numeric_limits<std::int64_t>::max());
    transition.timestampDomain = input::InputTimestampDomain::SteadyClock;
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
        const int lane = physicalScratchLane(transition.scope);
        for (std::size_t candidateIndex = 0;
             candidateIndex < transitions.size(); ++candidateIndex) {
          const auto &candidate = transitions[candidateIndex];
          const bool candidateIsScratch =
              candidate.action.kind ==
                  input::LogicalActionKind::ScratchClockwise ||
              candidate.action.kind ==
                  input::LogicalActionKind::ScratchCounterClockwise;
          if (!candidateIsScratch ||
              physicalScratchLane(candidate.scope) != lane) {
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
    if (lane >= 0) control_.releaseLane(lane, 0.0, false);
  }
  heldLaneOwners_.clear();
  scratchLaneStates_.clear();
  recordedScratchControls_.clear();
  pendingPhysicalEdges_.clear();
  latestPressedNote_ = nullptr;
}

int LogicalGameplayInputAdapter::physicalScratchLane(
    input::InputScope scope) {
  // Negative slots retain normal scratch ownership/reversal semantics without
  // sharing a note lane. In particular, lane 7 is a normal key in 8K.
  if (input_profile::usesCommandOnlyScratch(scope.keyMode)) {
    return scope.player == 2 ? -2 : -1;
  }
  return scope.player == 2 ? kSecondPlayerScratchLane : kFirstPlayerScratchLane;
}

bool LogicalGameplayInputAdapter::isLaneHeld(int lane) const {
  const auto scratch = scratchLaneStates_.find(lane);
  return heldLaneOwners_.contains(lane) ||
         (scratch != scratchLaneStates_.end() &&
          scratch->second.activeDirection.has_value());
}

bms_parser::Note *LogicalGameplayInputAdapter::pressPhysicalLane(int lane, std::uint64_t timestampMicros) {
  if (lane < 0) return nullptr;
  ++pendingPhysicalEdges_[lane];
  latestPressedNote_ = control_.pressLaneAt(lane, timestampMicros);
  return latestPressedNote_;
}

void LogicalGameplayInputAdapter::releasePhysicalLane(int lane,
                                                      bool backSpin,
                                                      std::uint64_t timestampMicros) {
  if (lane < 0) return;
  ++pendingPhysicalEdges_[lane];
  control_.releaseLaneAt(lane, timestampMicros, backSpin);
}

void LogicalGameplayInputAdapter::applyLane(
    const input::LogicalInputTransition &transition, OwnerKind ownerKind) {
  const int lane = transition.action.lane;
  if (!isValidGameplayLane(lane)) {
    return;
  }
  const bool wasHeld = isLaneHeld(lane);
  const replay::LogicalControl logicalControl = replayLaneControl(transition);
  const bool digitalScratch =
      replay::isDirectionalScratchControl(logicalControl.kind);
  const LaneOwner owner{.scope = transition.scope, .kind = ownerKind};
  if (transition.pressed) {
    const bool inserted = heldLaneOwners_[lane].insert(owner).second;
    if (inserted) {
      if (!wasHeld) {
        pressPhysicalLane(lane, transition.timestampMicros);
      }
      if (digitalScratch) {
        synchronizeScratchReplayControl(transition, lane);
      } else if (!wasHeld) {
        notifyApplied(transition, lane, logicalControl, true);
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
    releasePhysicalLane(lane, false, transition.timestampMicros);
    if (!digitalScratch) {
      notifyApplied(transition, lane, logicalControl, false);
    }
  }
  if (digitalScratch) {
    synchronizeScratchReplayControl(transition, lane);
  }
}

void LogicalGameplayInputAdapter::applyScratch(
    const input::LogicalInputTransition &transition, ScratchDirection direction,
    OwnerKind ownerKind, bool reversing, bool oppositeReleasedInBatch) {
  const int lane = physicalScratchLane(transition.scope);
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
      releasePhysicalLane(lane, true, transition.timestampMicros);
      pressPhysicalLane(lane, transition.timestampMicros);
      synchronizeScratchReplayControl(transition, lane);
      return;
    }

    state.activeDirection.reset();
    const bool digitalLaneHeld = heldLaneOwners_.contains(lane);
    if (reversing || !digitalLaneHeld) {
      releasePhysicalLane(lane, reversing, transition.timestampMicros);
      if (reversing && digitalLaneHeld) {
        pressPhysicalLane(lane, transition.timestampMicros);
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
    releasePhysicalLane(lane, true, transition.timestampMicros);
    pressPhysicalLane(lane, transition.timestampMicros);
    state.activeDirection = direction;
    synchronizeScratchReplayControl(transition, lane);
    return;
  }
  state.activeDirection = direction;
  if (!wasHeld) {
    pressPhysicalLane(lane, transition.timestampMicros);
  }
  synchronizeScratchReplayControl(transition, lane);
}

replay::LogicalControl LogicalGameplayInputAdapter::replayLaneControl(
    const input::LogicalInputTransition &transition) {
  const bool digitalScratch =
      (transition.scope.keyMode == 5 || transition.scope.keyMode == 7 ||
       transition.scope.keyMode == 10 || transition.scope.keyMode == 14) &&
      transition.action.lane == physicalScratchLane(transition.scope);
  const auto control = replay::logicalControlForChartLane(
      input_profile::canonicalChartKeyMode(transition.scope.keyMode),
      transition.action.lane, digitalScratch);
  return control.value_or(replay::LogicalControl{
      .kind = replay::LogicalControlKind::Lane,
      .player = transition.scope.player,
      .lane = -1});
}

std::optional<replay::LogicalControl>
LogicalGameplayInputAdapter::effectiveScratchReplayControl(int lane) const {
  const int player = lane == kSecondPlayerScratchLane || lane == -2 ? 2 : 1;
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
    notifyApplied(transition, lane, *previous, false);
  }
  if (next.has_value()) {
    notifyApplied(transition, lane, *next, true);
    recordedScratchControls_[lane] = *next;
  } else {
    recordedScratchControls_.erase(lane);
  }
}

void LogicalGameplayInputAdapter::notifyApplied(
    const input::LogicalInputTransition &source,
    int physicalLane, replay::LogicalControl control, bool pressed) {
  if (physicalLane < 0 && scratchCommandControl(source).has_value()) {
    if (commandCallback_) {
      auto command = source;
      command.action.kind = control.kind == replay::LogicalControlKind::ScratchClockwise
                                ? input::LogicalActionKind::ScratchClockwise
                                : input::LogicalActionKind::ScratchCounterClockwise;
      command.pressed = pressed;
      commandCallback_(command);
    }
    return;
  }
  const auto pending = pendingPhysicalEdges_.find(physicalLane);
  const bool replayOnly = pending == pendingPhysicalEdges_.end();
  if (!replayOnly && --pending->second == 0) {
    pendingPhysicalEdges_.erase(pending);
  }
  const auto mappedLane = replay::physicalChartLaneForLogicalControl(
      input_profile::canonicalChartKeyMode(source.scope.keyMode), control);
  const bool hasReplayControl =
      mappedLane.has_value() && *mappedLane == physicalLane;
  if (appliedTransitionCallback_) {
    appliedTransitionCallback_({.source = source,
                                .physicalLane = physicalLane,
                                .control = control,
                                .hasReplayControl = hasReplayControl,
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
                              .physicalLane = -1,
                              .control = {.kind = kind,
                                          .player = transition.scope.player,
                                          .lane = -1},
                              .hasReplayControl = true,
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

bms_parser::Note *LogicalGameplayInputPipeline::consumePhysicalTouchLane(
    input::InputScope scope, int lane, bool pressed,
    std::optional<int> scratchDirection, std::uint64_t timestampMicros) {
  const input::LogicalActionKind action =
      !scratchDirection.has_value()
          ? input::LogicalActionKind::Lane
          : *scratchDirection > 0
                ? input::LogicalActionKind::ScratchClockwise
                : input::LogicalActionKind::ScratchCounterClockwise;
  return consumeTouchTransition({
      .scope = scope,
      .action = {.kind = action, .lane = lane},
      .pressed = pressed,
      .value = pressed ? 1.0F : 0.0F,
      .timestampMicros = timestampMicros,
  });
}

void LogicalGameplayInputPipeline::setBindings(
    const InputProfile &profile, std::vector<input::InputScope> activeScopes) {
  reset();
  resolver_ = InputBindingResolver(
      profile, std::move(activeScopes),
      {.onTransitions = [this](auto transitions) { adapter_.apply(transitions); }});
}

void LogicalGameplayInputPipeline::disconnectDevice(std::string_view stableId) {
  resolver_.disconnectDevice(stableId);
}

void LogicalGameplayInputPipeline::reset() {
  resolver_.reset();
  adapter_.reset();
}
