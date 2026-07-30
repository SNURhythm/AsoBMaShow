#include "RealtimePhysicalInputRouter.h"

#include <algorithm>
#include <utility>

namespace input {

RealtimePhysicalInputRouter::RealtimePhysicalInputRouter(
    const InputProfile &profile, std::vector<InputScope> activeScopes,
    Sink sink)
    : sink_(std::move(sink)),
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
  for (std::size_t lane = 0; lane < desiredLanePressed_.size(); ++lane) {
    if (desiredLanePressed_[lane] != publishedLanePressed_[lane]) {
      const auto &replayControl = desiredLanePressed_[lane]
                                      ? desiredReplayControls_[lane]
                                      : publishedReplayControls_[lane];
      (void)emit(desiredLanePressed_[lane]
                     ? RealtimePhysicalInputTransitionType::Press
                     : RealtimePhysicalInputTransitionType::Release,
                 static_cast<int>(lane), false, replayControl);
      continue;
    }
    if (!desiredLanePressed_[lane] ||
        desiredReplayControls_[lane] == publishedReplayControls_[lane]) {
      continue;
    }
    if (publishedReplayControls_[lane].has_value()) {
      (void)emitReplayOnly(RealtimePhysicalInputTransitionType::Release,
                           *publishedReplayControls_[lane]);
    }
    if (desiredReplayControls_[lane].has_value()) {
      (void)emitReplayOnly(RealtimePhysicalInputTransitionType::Press,
                           *desiredReplayControls_[lane]);
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
            desiredReplayControls_.size()) {
      desiredReplayControls_[static_cast<std::size_t>(applied.physicalLane)] =
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
  const bool tracked = lane >= 0 &&
                       static_cast<std::size_t>(lane) <
                           desiredLanePressed_.size();
  if (tracked) {
    desiredLanePressed_[static_cast<std::size_t>(lane)] =
        type == RealtimePhysicalInputTransitionType::Press;
    desiredReplayControls_[static_cast<std::size_t>(lane)] = replayControl;
  }
  if (!gameplayEnabled_ || !sink_) {
    return true;
  }
  if (tracked &&
      desiredLanePressed_[static_cast<std::size_t>(lane)] ==
          publishedLanePressed_[static_cast<std::size_t>(lane)]) {
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
  if (published && tracked) {
    publishedLanePressed_[static_cast<std::size_t>(lane)] =
        desiredLanePressed_[static_cast<std::size_t>(lane)];
    publishedReplayControls_[static_cast<std::size_t>(lane)] =
        replayControl;
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
