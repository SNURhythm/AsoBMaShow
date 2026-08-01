#include "RealtimeGameplayInputBridge.h"

#include <algorithm>

namespace gameplay {

RealtimeGameplayInputBridge::RealtimeGameplayInputBridge(
    std::uint64_t epoch, RealtimeGameplayInputBridgeSink sink) noexcept
    : epoch_(epoch), sink_(sink) {}

bool RealtimeGameplayInputBridge::prepare(
    RealtimeGameplayInputType type, int lane, int compensateLane,
    bool backSpin, std::int64_t steadyTimestampMicros,
    std::int64_t inputDelayMicros) {
  if (sink_.emit == nullptr) {
    return false;
  }
  const std::lock_guard lock(mutex_);
  pendingInputs_.push_back({.epoch = epoch_,
                            .type = type,
                            .source =
                                RealtimeGameplayInputSource::LegacyAdapter,
                            .lane = lane,
                            .compensateLane = compensateLane,
                            .backSpin = backSpin,
                            .steadyTimestampMicros = steadyTimestampMicros,
                            .inputDelayMicros = inputDelayMicros});
  return true;
}

bool RealtimeGameplayInputBridge::emitApplied(
    int physicalLane, replay::LogicalControl control, bool hasReplayControl,
    bool pressed, bool replayOnly, std::int64_t steadyTimestampMicros) {
  if (sink_.emit == nullptr) {
    return false;
  }
  RealtimeGameplayInput input{
      .epoch = epoch_,
      .type = pressed ? RealtimeGameplayInputType::Press
                      : RealtimeGameplayInputType::Release,
      .source = RealtimeGameplayInputSource::LegacyAdapter,
      .lane = physicalLane,
      .compensateLane = physicalLane,
      .steadyTimestampMicros = steadyTimestampMicros,
      .hasReplayControl = hasReplayControl,
      .replayControl = control,
      .replayOnly = true,
  };
  {
    const std::lock_guard lock(mutex_);
    if (!replayOnly && physicalLane >= 0) {
      const auto pending = std::ranges::find_if(
          pendingInputs_, [&](const auto &candidate) {
            return candidate.lane == physicalLane &&
                   candidate.type == input.type;
          });
      if (pending != pendingInputs_.end()) {
        input = *pending;
        pendingInputs_.erase(pending);
        input.hasReplayControl = hasReplayControl;
        input.replayControl = control;
        input.replayOnly = false;
      }
    } else if (replayOnly && hasReplayControl &&
               replay::isDirectionalScratchControl(control.kind) &&
               control.player >= 1 &&
               static_cast<std::size_t>(control.player) <
                   pendingScratchHandoffs_.size()) {
      auto &handoff = pendingScratchHandoffs_[
          static_cast<std::size_t>(control.player)];
      if (!pressed) {
        handoff = {.released = control,
                   .steadyTimestampMicros = steadyTimestampMicros,
                   .active = true};
      } else if (handoff.active && handoff.released.kind != control.kind) {
        input.steadyTimestampMicros = handoff.steadyTimestampMicros;
        handoff = {};
      }
    }
  }
  return sink_.emit(sink_.context, input);
}

} // namespace gameplay
