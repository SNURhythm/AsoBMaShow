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
                }) {}

void RealtimePhysicalInputRouter::consume(
    const PhysicalInputEvent &event, std::int64_t steadyTimestampMicros) {
  const std::lock_guard lock(mutex_);
  currentTimestampMicros_ = steadyTimestampMicros;
  (void)pipeline_.consumeRegistryEvent(event);
}

void RealtimePhysicalInputRouter::setGameplayEnabled(
    bool enabled, std::int64_t steadyTimestampMicros) {
  const std::lock_guard lock(mutex_);
  if (gameplayEnabled_ == enabled) {
    return;
  }
  currentTimestampMicros_ = steadyTimestampMicros;
  pipeline_.reset();
  gameplayEnabled_ = enabled;
}

bms_parser::Note *RealtimePhysicalInputRouter::pressLane(
    int mainLane, int compensateLane, double inputDelay) {
  (void)compensateLane;
  (void)inputDelay;
  (void)emit(RealtimePhysicalInputTransitionType::Press, mainLane);
  return nullptr;
}

bms_parser::Note *RealtimePhysicalInputRouter::pressLane(int lane,
                                                         double inputDelay) {
  (void)inputDelay;
  (void)emit(RealtimePhysicalInputTransitionType::Press, lane);
  return nullptr;
}

bms_parser::Note *RealtimePhysicalInputRouter::releaseLane(
    int lane, double inputDelay, bool isBackSpin) {
  (void)inputDelay;
  (void)emit(RealtimePhysicalInputTransitionType::Release, lane, isBackSpin);
  return nullptr;
}

bool RealtimePhysicalInputRouter::emit(
    RealtimePhysicalInputTransitionType type, int lane, bool backSpin) {
  return !gameplayEnabled_ || !sink_ ||
         sink_({.type = type,
                .lane = lane,
                .backSpin = backSpin,
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
