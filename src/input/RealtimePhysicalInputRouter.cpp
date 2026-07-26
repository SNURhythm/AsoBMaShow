#include "RealtimePhysicalInputRouter.h"

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
  pipeline_.disconnectDevice(deviceId, steadyTimestampMicros);
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
    if (desiredLanePressed_[lane] == publishedLanePressed_[lane]) {
      continue;
    }
    (void)emit(desiredLanePressed_[lane]
                   ? RealtimePhysicalInputTransitionType::Press
                   : RealtimePhysicalInputTransitionType::Release,
               static_cast<int>(lane), false, desiredReplayControls_[lane]);
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
  if (pendingTransitions_.empty()) {
    return;
  }
  auto pending = std::move(pendingTransitions_.front());
  pendingTransitions_.pop_front();
  pending.replayControl = applied.control.kind;
  (void)emit(pending.type, pending.lane, pending.backSpin,
             pending.replayControl);
}

bool RealtimePhysicalInputRouter::emit(
    RealtimePhysicalInputTransitionType type, int lane, bool backSpin,
    replay::LogicalControlKind replayControl) {
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
                                .replayControl = replayControl,
                                .steadyTimestampMicros =
                                    currentTimestampMicros_});
  if (published && tracked) {
    publishedLanePressed_[static_cast<std::size_t>(lane)] =
        desiredLanePressed_[static_cast<std::size_t>(lane)];
  }
  return published;
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
