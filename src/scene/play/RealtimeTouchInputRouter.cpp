#include "RealtimeTouchInputRouter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gameplay {
namespace {
constexpr std::int64_t kCancelledTouchGraceMicros = 50'000;
}

RealtimeTouchInputRouter::RealtimeTouchInputRouter(
    std::uint64_t epoch, RealtimeTouchLayout layout,
    RealtimeTouchInputSink sink) noexcept
    : epoch_(epoch), layout_(std::move(layout)), sink_(sink) {
  layout_.laneCount =
      std::min(layout_.laneCount, kRealtimeTouchLaneCapacity);
}

std::optional<std::size_t>
RealtimeTouchInputRouter::laneIndexAt(float x, float y,
                                      bool requireInside) const noexcept {
  if (layout_.laneCount == 0 || !std::isfinite(x) || !std::isfinite(y)) {
    return std::nullopt;
  }
  const float bottomY =
      (layout_.bottomLeft.y + layout_.bottomRight.y) * 0.5F;
  const float topY = (layout_.topLeft.y + layout_.topRight.y) * 0.5F;
  const float height = topY - bottomY;
  if (std::abs(height) <= 0.000001F) {
    return std::nullopt;
  }
  float vertical = (y - bottomY) / height;
  if (requireInside && (vertical < 0.0F || vertical > 1.0F)) {
    return std::nullopt;
  }
  vertical = std::clamp(vertical, 0.0F, 1.0F);
  const float left = std::lerp(layout_.bottomLeft.x, layout_.topLeft.x,
                               vertical);
  const float right = std::lerp(layout_.bottomRight.x, layout_.topRight.x,
                                vertical);
  const float width = right - left;
  if (std::abs(width) <= 0.000001F) {
    return std::nullopt;
  }
  float horizontal = (x - left) / width;
  if (requireInside && (horizontal < 0.0F || horizontal >= 1.0F)) {
    return std::nullopt;
  }
  horizontal = std::clamp(horizontal, 0.0F,
                          std::nextafter(1.0F, 0.0F));
  return std::min(static_cast<std::size_t>(
                      horizontal * static_cast<float>(layout_.laneCount)),
                  layout_.laneCount - 1);
}

RealtimeTouchInputRouter::FingerState *
RealtimeTouchInputRouter::findFinger(std::int64_t fingerId) noexcept {
  for (auto &finger : fingers_) {
    if (finger.active && finger.fingerId == fingerId) {
      return &finger;
    }
  }
  return nullptr;
}

RealtimeTouchInputRouter::FingerState *
RealtimeTouchInputRouter::allocateFinger(std::int64_t fingerId) noexcept {
  if (auto *existing = findFinger(fingerId); existing != nullptr) {
    return existing;
  }
  for (auto &finger : fingers_) {
    if (!finger.active) {
      finger = {};
      finger.active = true;
      finger.fingerId = fingerId;
      return &finger;
    }
  }
  return nullptr;
}

bool RealtimeTouchInputRouter::laneOccupied(
    int lane, std::int64_t exceptFinger) const noexcept {
  return std::ranges::any_of(fingers_, [&](const FingerState &finger) {
    return finger.active && finger.fingerId != exceptFinger &&
           finger.lane == lane;
  });
}

bool RealtimeTouchInputRouter::emit(RealtimeGameplayInputType type, int lane,
                                    replay::LogicalControl replayControl,
                                    std::int64_t timestampMicros,
                                    bool backSpin) noexcept {
  return sink_.emit != nullptr &&
         sink_.emit(sink_.context,
                    {.epoch = epoch_,
                     .type = type,
                     .source = RealtimeGameplayInputSource::Touch,
                     .lane = lane,
                     .compensateLane = lane,
                     .backSpin = backSpin,
                     .steadyTimestampMicros = timestampMicros,
                     .hasReplayControl = true,
                     .replayControl = replayControl});
}

bool RealtimeTouchInputRouter::beginLane(
    FingerState &finger, std::size_t laneIndex,
    const RealtimeTouchSample &sample) noexcept {
  if (laneIndex >= layout_.laneCount) {
    return false;
  }
  const int lane = layout_.lanes[laneIndex];
  if (laneOccupied(lane, finger.fingerId)) {
    return false;
  }
  finger.lane = lane;
  finger.scratch = layout_.scratch[laneIndex];
  const auto replayControl = replay::logicalControlForChartLane(
      layout_.keyMode, lane, finger.scratch);
  if (!replayControl.has_value()) {
    finger.lane = -1;
    return false;
  }
  finger.replayControl = *replayControl;
  finger.pressed = false;
  finger.scratchDirection = 0;
  finger.lastX = sample.normalizedX;
  finger.lastY = sample.normalizedY;
  finger.cancelDeadlineMicros = 0;
  if (finger.scratch) {
    return true;
  }
  if (!emit(RealtimeGameplayInputType::Press, lane, finger.replayControl,
            sample.steadyTimestampMicros)) {
    finger.lane = -1;
    return false;
  }
  finger.pressed = true;
  return true;
}

bool RealtimeTouchInputRouter::releaseLane(FingerState &finger,
                                           std::int64_t timestampMicros,
                                           bool backSpin) noexcept {
  const int lane = finger.lane;
  const bool shouldEmit = lane >= 0 && finger.pressed;
  const auto replayControl = finger.replayControl;
  finger.lane = -1;
  finger.pressed = false;
  finger.scratch = false;
  finger.scratchDirection = 0;
  finger.cancelDeadlineMicros = 0;
  if (!shouldEmit) {
    return true;
  }
  return emit(RealtimeGameplayInputType::Release, lane, replayControl,
              timestampMicros,
              backSpin);
}

bool RealtimeTouchInputRouter::handleScratchMove(
    FingerState &finger, const RealtimeTouchSample &sample) noexcept {
  const float dx = sample.normalizedX - finger.lastX;
  const float dy = sample.normalizedY - finger.lastY;
  finger.lastX = sample.normalizedX;
  finger.lastY = sample.normalizedY;
  const float distance = std::sqrt(dx * dx + dy * dy);
  const bool longNoteHeld =
      sink_.scratchLongNoteHeld != nullptr &&
      sink_.scratchLongNoteHeld(sink_.context, finger.lane);
  const float threshold = finger.scratchDirection == 0
                              ? 0.001F
                              : longNoteHeld ? 0.01F : 0.002F;
  if (distance <= threshold) {
    return true;
  }
  const int direction = dy < 0.0F ? 1 : -1;
  if (direction == finger.scratchDirection) {
    return true;
  }
  if (finger.pressed &&
      !emit(RealtimeGameplayInputType::Release, finger.lane,
            finger.replayControl, sample.steadyTimestampMicros, true)) {
    return false;
  }
  finger.pressed = false;
  const auto replayControl = replay::logicalControlForChartLane(
      layout_.keyMode, finger.lane, true,
      direction > 0 ? replay::LogicalControlKind::ScratchClockwise
                    : replay::LogicalControlKind::ScratchCounterClockwise);
  if (!replayControl.has_value() ||
      !emit(RealtimeGameplayInputType::Press, finger.lane, *replayControl,
            sample.steadyTimestampMicros)) {
    return false;
  }
  finger.pressed = true;
  finger.scratchDirection = direction;
  finger.replayControl = *replayControl;
  return true;
}

bool RealtimeTouchInputRouter::consume(
    const RealtimeTouchSample &sample) noexcept {
  if (!gameplayEnabled_) {
    return true;
  }
  switch (sample.phase) {
  case RealtimeTouchPhase::Down: {
    auto *finger = allocateFinger(sample.fingerId);
    if (finger == nullptr) {
      return false;
    }
    if (finger->cancelDeadlineMicros != 0) {
      finger->cancelDeadlineMicros = 0;
      finger->lastX = sample.normalizedX;
      finger->lastY = sample.normalizedY;
      return true;
    }
    if (sample.excludedFromGameplay) {
      finger->excluded = true;
      return true;
    }
    const auto lane = laneIndexAt(sample.normalizedX, sample.normalizedY,
                                  layout_.dragMode);
    if (!lane.has_value()) {
      finger->active = false;
      return true;
    }
    if (!beginLane(*finger, *lane, sample)) {
      finger->active = false;
      return false;
    }
    return true;
  }
  case RealtimeTouchPhase::Move: {
    auto *finger = findFinger(sample.fingerId);
    if (finger == nullptr) {
      if (!layout_.dragMode) {
        return true;
      }
      finger = allocateFinger(sample.fingerId);
      if (finger == nullptr) {
        return false;
      }
    }
    finger->cancelDeadlineMicros = 0;
    if (finger->excluded) {
      return true;
    }
    if (sample.excludedFromGameplay) {
      const bool released =
          releaseLane(*finger, sample.steadyTimestampMicros);
      finger->excluded = true;
      return released;
    }
    if (!layout_.dragMode) {
      return !finger->scratch || handleScratchMove(*finger, sample);
    }
    const auto lane = laneIndexAt(sample.normalizedX, sample.normalizedY, true);
    if (!lane.has_value()) {
      return releaseLane(*finger, sample.steadyTimestampMicros);
    }
    const int nextLane = layout_.lanes[*lane];
    if (finger->lane == nextLane) {
      return !finger->scratch || handleScratchMove(*finger, sample);
    }
    if (!releaseLane(*finger, sample.steadyTimestampMicros)) {
      return false;
    }
    return beginLane(*finger, *lane, sample);
  }
  case RealtimeTouchPhase::Up: {
    auto *finger = findFinger(sample.fingerId);
    if (finger == nullptr) {
      return true;
    }
    const bool released =
        finger->excluded
            ? true
            : releaseLane(*finger, sample.steadyTimestampMicros);
    finger->active = false;
    return released;
  }
  case RealtimeTouchPhase::Cancel: {
    auto *finger = findFinger(sample.fingerId);
    if (finger == nullptr) {
      return true;
    }
    finger->cancelDeadlineMicros =
        sample.steadyTimestampMicros >
                std::numeric_limits<std::int64_t>::max() -
                    kCancelledTouchGraceMicros
            ? std::numeric_limits<std::int64_t>::max()
            : sample.steadyTimestampMicros + kCancelledTouchGraceMicros;
    return true;
  }
  case RealtimeTouchPhase::CancelExpired: {
    auto *finger = findFinger(sample.fingerId);
    if (finger == nullptr || finger->cancelDeadlineMicros == 0 ||
        sample.steadyTimestampMicros < finger->cancelDeadlineMicros) {
      return true;
    }
    const bool released =
        finger->excluded
            ? true
            : releaseLane(*finger, sample.steadyTimestampMicros);
    finger->cancelDeadlineMicros = 0;
    finger->active = false;
    return released;
  }
  }
  return false;
}

void RealtimeTouchInputRouter::setGameplayEnabled(bool enabled) noexcept {
  gameplayEnabled_ = enabled;
}

void RealtimeTouchInputRouter::reset() noexcept {
  for (auto &finger : fingers_) {
    finger = {};
  }
}

bool RealtimeTouchInputRouter::cancelAll(
    std::int64_t steadyTimestampMicros) noexcept {
  bool success = true;
  for (auto &finger : fingers_) {
    if (!finger.active) {
      continue;
    }
    success = releaseLane(finger, steadyTimestampMicros) && success;
    finger.active = false;
  }
  return success;
}

bool RealtimeTouchInputRouter::updateLayout(
    RealtimeTouchLayout layout,
    std::int64_t steadyTimestampMicros) noexcept {
  const bool released = cancelAll(steadyTimestampMicros);
  layout.laneCount =
      std::min(layout.laneCount, kRealtimeTouchLaneCapacity);
  layout_ = std::move(layout);
  return released;
}

} // namespace gameplay
