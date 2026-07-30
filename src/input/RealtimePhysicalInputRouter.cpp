#include "RealtimePhysicalInputRouter.h"

#include <algorithm>
#include <span>
#include <utility>

namespace input {
namespace {

std::size_t trackedLaneExtent(const InputProfile &profile,
                              std::span<const InputScope> activeScopes) {
  std::size_t extent = 0;
  for (const auto &binding : profile.bindings) {
    if (std::ranges::find(activeScopes, binding.scope) == activeScopes.end()) {
      continue;
    }
    int lane = -1;
    if (binding.action.kind == LogicalActionKind::Lane) {
      lane = binding.action.lane;
    } else if (binding.action.kind == LogicalActionKind::ScratchClockwise ||
               binding.action.kind ==
                   LogicalActionKind::ScratchCounterClockwise) {
      lane = LogicalGameplayInputAdapter::physicalScratchLane(binding.scope);
    }
    if (lane >= 0) {
      extent = std::max(extent, static_cast<std::size_t>(lane) + 1);
    }
  }
  return extent;
}

} // namespace

RealtimePhysicalInputRouter::RealtimePhysicalInputRouter(
    const InputProfile &profile, std::vector<InputScope> activeScopes,
    Sink sink)
    : sink_(std::move(sink)),
      trackedLanes_(trackedLaneExtent(profile, activeScopes)),
      pipeline_(*this, profile, std::move(activeScopes),
                [this](const LogicalInputTransition &transition) {
                  emitCommand(transition);
                }, {},
                [this](const auto &transition) { emitApplied(transition); }) {}

void RealtimePhysicalInputRouter::consume(
    const PhysicalInputEvent &event, std::int64_t steadyTimestampMicros) {
  const std::lock_guard lock(mutex_);
  currentTimestampMicros_ = steadyTimestampMicros;
  (void)pipeline_.consumeRegistryEvent(event);
}

void RealtimePhysicalInputRouter::disconnectDevice(
    std::string_view deviceId, std::int64_t steadyTimestampMicros) {
  const std::lock_guard lock(mutex_);
  currentTimestampMicros_ = steadyTimestampMicros;
  pipeline_.disconnectDevice(deviceId);
}

void RealtimePhysicalInputRouter::setGameplayEnabled(
    bool enabled, std::int64_t steadyTimestampMicros) {
  const std::lock_guard lock(mutex_);
  if (gameplayEnabled_ == enabled) {
    return;
  }
  currentTimestampMicros_ = steadyTimestampMicros;
  gameplayEnabled_ = enabled;
  if (!enabled) {
    return;
  }
  for (std::size_t lane = 0; lane < trackedLanes_.size(); ++lane) {
    const auto &state = trackedLanes_[lane];
    if (state.desiredPressed != state.publishedPressed) {
      const auto &replayControl = state.desiredPressed
                                      ? state.desiredReplayControl
                                      : state.publishedReplayControl;
      (void)emit(state.desiredPressed
                     ? RealtimePhysicalInputTransitionType::Press
                     : RealtimePhysicalInputTransitionType::Release,
                 static_cast<int>(lane), false, replayControl);
      continue;
    }
    if (!state.desiredPressed ||
        state.desiredReplayControl == state.publishedReplayControl) {
      continue;
    }
    if (state.publishedReplayControl.has_value()) {
      (void)emitReplayOnly(RealtimePhysicalInputTransitionType::Release,
                           *state.publishedReplayControl);
    }
    if (state.desiredReplayControl.has_value()) {
      (void)emitReplayOnly(RealtimePhysicalInputTransitionType::Press,
                           *state.desiredReplayControl);
    }
  }
}

bms_parser::Note *RealtimePhysicalInputRouter::pressLane(
    int mainLane, int compensateLane, double inputDelay) {
  (void)compensateLane;
  (void)inputDelay;
  prepare(RealtimePhysicalInputTransitionType::Press, mainLane);
  return nullptr;
}

bms_parser::Note *RealtimePhysicalInputRouter::pressLane(int lane,
                                                         double inputDelay) {
  (void)inputDelay;
  prepare(RealtimePhysicalInputTransitionType::Press, lane);
  return nullptr;
}

bms_parser::Note *RealtimePhysicalInputRouter::releaseLane(
    int lane, double inputDelay, bool isBackSpin) {
  (void)inputDelay;
  prepare(RealtimePhysicalInputTransitionType::Release, lane, isBackSpin);
  return nullptr;
}

void RealtimePhysicalInputRouter::prepare(
    RealtimePhysicalInputTransitionType type, int lane, bool backSpin) {
  pendingTransitions_.push_back(
      {.type = type, .lane = lane, .backSpin = backSpin});
}

void RealtimePhysicalInputRouter::emitApplied(
    const LogicalGameplayInputAdapter::AppliedTransition &applied) {
  const auto pending = std::ranges::find(
      pendingTransitions_, applied.physicalLane,
      &RealtimePhysicalInputTransition::lane);
  if (applied.replayOnly || pending == pendingTransitions_.end()) {
    if (applied.hasReplayControl) {
      (void)emitReplayOnly(applied.pressed
                               ? RealtimePhysicalInputTransitionType::Press
                               : RealtimePhysicalInputTransitionType::Release,
                           applied.control);
    }
    if (applied.pressed && applied.physicalLane >= 0 &&
        static_cast<std::size_t>(applied.physicalLane) <
            trackedLanes_.size()) {
      trackedLanes_[static_cast<std::size_t>(applied.physicalLane)]
          .desiredReplayControl =
          applied.hasReplayControl
              ? std::optional<replay::LogicalControl>(applied.control)
              : std::nullopt;
    }
    return;
  }
  auto physical = std::move(*pending);
  pendingTransitions_.erase(pending);
  (void)emit(physical.type, physical.lane, physical.backSpin,
             applied.hasReplayControl
                 ? std::optional<replay::LogicalControl>(applied.control)
                 : std::nullopt);
}

bool RealtimePhysicalInputRouter::emit(
    RealtimePhysicalInputTransitionType type, int lane, bool backSpin,
    std::optional<replay::LogicalControl> replayControl) {
  TrackedLaneState *tracked =
      lane >= 0 && static_cast<std::size_t>(lane) < trackedLanes_.size()
          ? &trackedLanes_[static_cast<std::size_t>(lane)]
          : nullptr;
  if (tracked != nullptr) {
    tracked->desiredPressed =
        type == RealtimePhysicalInputTransitionType::Press;
    tracked->desiredReplayControl = replayControl;
  }
  if (!gameplayEnabled_ || !sink_) {
    return true;
  }
  if (tracked != nullptr &&
      tracked->desiredPressed == tracked->publishedPressed) {
    return true;
  }
  const bool published = sink_({.type = type,
                                .lane = lane,
                                .backSpin = backSpin,
                                .hasReplayControl = replayControl.has_value(),
                                .replayControl = replayControl.value_or(
                                    replay::LogicalControl{}),
                                .steadyTimestampMicros =
                                    currentTimestampMicros_});
  if (published && tracked != nullptr) {
    tracked->publishedPressed = tracked->desiredPressed;
    tracked->publishedReplayControl = replayControl;
  }
  return published;
}

bool RealtimePhysicalInputRouter::emitReplayOnly(
    RealtimePhysicalInputTransitionType type,
    replay::LogicalControl replayControl) {
  if (!gameplayEnabled_ || !sink_) {
    return true;
  }
  return sink_({.type = type,
                .lane = -1,
                .hasReplayControl = true,
                .replayControl = replayControl,
                .replayOnly = true,
                .steadyTimestampMicros = currentTimestampMicros_});
}

void RealtimePhysicalInputRouter::emitCommand(
    const LogicalInputTransition &transition) {
  if (!sink_) {
    return;
  }
  (void)sink_({.type = RealtimePhysicalInputTransitionType::Command,
               .command = transition,
               .steadyTimestampMicros = currentTimestampMicros_});
}

} // namespace input
