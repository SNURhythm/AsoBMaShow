#include "RealtimeTouchInputRouter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gameplay {
namespace {
constexpr std::int64_t kCancelledTouchGraceMicros = 50'000;
constexpr float kHitTestEpsilon = 0.000001F;

bool isFinite(const RealtimeTouchPoint &point) noexcept {
  return std::isfinite(point.x) && std::isfinite(point.y);
}

float cross(const RealtimeTouchPoint &from, const RealtimeTouchPoint &to,
            const RealtimeTouchPoint &point) noexcept {
  return (to.x - from.x) * (point.y - from.y) -
         (to.y - from.y) * (point.x - from.x);
}

bool contains(const RealtimeTouchLaneRegion &region,
              const RealtimeTouchPoint &point) noexcept {
  if (!isFinite(point) || !isFinite(region.bottomLeft) ||
      !isFinite(region.bottomRight) || !isFinite(region.topLeft) ||
      !isFinite(region.topRight)) {
    return false;
  }
  const std::array points{region.bottomLeft, region.bottomRight,
                          region.topRight, region.topLeft};
  int winding = 0;
  for (std::size_t index = 0; index < points.size(); ++index) {
    const float side = cross(points[index], points[(index + 1) % points.size()],
                             point);
    if (std::abs(side) <= kHitTestEpsilon) {
      continue;
    }
    const int sideWinding = side > 0.0F ? 1 : -1;
    if (winding != 0 && winding != sideWinding) {
      return false;
    }
    winding = sideWinding;
  }
  return winding != 0;
}

RealtimeTouchPoint clampedVertically(const RealtimeTouchLaneRegion &region,
                                     RealtimeTouchPoint point) noexcept {
  const auto yRange = std::minmax({region.bottomLeft.y, region.bottomRight.y,
                                   region.topLeft.y, region.topRight.y});
  point.y = std::clamp(point.y, yRange.first, yRange.second);
  return point;
}
}

RealtimeTouchInputRouter::RealtimeTouchInputRouter(
    std::uint64_t epoch, RealtimeTouchLayout layout,
    RealtimeTouchInputSink sink) noexcept
    : epoch_(epoch), layout_(std::move(layout)), sink_(sink) {
  legacyUniformLayout_ = normalizeLayout(layout_);
}

std::optional<std::size_t>
RealtimeTouchInputRouter::laneIndexAt(float x, float y,
                                      bool requireInside) const noexcept {
  if (layout_.laneRegions.empty() || !std::isfinite(x) || !std::isfinite(y)) {
    return std::nullopt;
  }
  const RealtimeTouchPoint point{x, y};
  for (std::size_t index = 0; index < layout_.laneRegions.size(); ++index) {
    if (contains(layout_.laneRegions[index], point)) {
      return index;
    }
  }
  if (requireInside) {
    return std::nullopt;
  }
  if (legacyUniformLayout_) {
    const float bottomY =
        (layout_.bottomLeft.y + layout_.bottomRight.y) * 0.5F;
    const float topY =
        (layout_.topLeft.y + layout_.topRight.y) * 0.5F;
    const float height = topY - bottomY;
    if (std::abs(height) <= kHitTestEpsilon) {
      return std::nullopt;
    }
    const float vertical = std::clamp((y - bottomY) / height, 0.0F, 1.0F);
    const float left =
        std::lerp(layout_.bottomLeft.x, layout_.topLeft.x, vertical);
    const float right =
        std::lerp(layout_.bottomRight.x, layout_.topRight.x, vertical);
    const float width = right - left;
    if (std::abs(width) <= kHitTestEpsilon) {
      return std::nullopt;
    }
    const float horizontal = std::clamp(
        (x - left) / width, 0.0F, std::nextafter(1.0F, 0.0F));
    const std::size_t originalIndex = std::min(
        static_cast<std::size_t>(
            horizontal * static_cast<float>(layout_.laneCount)),
        layout_.laneCount - 1);
    return layout_.laneCount - 1 - originalIndex;
  }
  for (std::size_t index = 0; index < layout_.laneRegions.size(); ++index) {
    if (contains(layout_.laneRegions[index],
                 clampedVertically(layout_.laneRegions[index], point))) {
      return index;
    }
  }
  return std::nullopt;
}

bool RealtimeTouchInputRouter::normalizeLayout(
    RealtimeTouchLayout &layout) noexcept {
  if (!layout.laneRegions.empty()) {
    layout.laneCount = layout.laneRegions.size();
    return false;
  }
  layout.laneCount =
      std::min({layout.laneCount, layout.lanes.size(), layout.scratch.size()});
  layout.laneRegions.reserve(layout.laneCount);
  // The historical uniform layout selects a shared boundary's lane on its
  // right. Reverse adapter insertion retains that behavior while authored
  // regions use their explicit vector order.
  for (std::size_t reverse = layout.laneCount; reverse > 0; --reverse) {
    const std::size_t index = reverse - 1;
    const float left = static_cast<float>(index) /
                       static_cast<float>(layout.laneCount);
    const float right = static_cast<float>(index + 1) /
                        static_cast<float>(layout.laneCount);
    layout.laneRegions.push_back(
        {.bottomLeft = {std::lerp(layout.bottomLeft.x, layout.bottomRight.x,
                                  left),
                        std::lerp(layout.bottomLeft.y, layout.bottomRight.y,
                                  left)},
         .bottomRight = {std::lerp(layout.bottomLeft.x, layout.bottomRight.x,
                                   right),
                         std::lerp(layout.bottomLeft.y, layout.bottomRight.y,
                                   right)},
         .topLeft = {std::lerp(layout.topLeft.x, layout.topRight.x, left),
                     std::lerp(layout.topLeft.y, layout.topRight.y, left)},
         .topRight = {std::lerp(layout.topLeft.x, layout.topRight.x, right),
                      std::lerp(layout.topLeft.y, layout.topRight.y, right)},
         .lane = layout.lanes[index],
         .scratch = layout.scratch[index]});
  }
  return true;
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
                                    std::optional<replay::LogicalControl>
                                        replayControl,
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
                     .hasReplayControl = replayControl.has_value(),
                     .replayControl = replayControl.value_or(
                         replay::LogicalControl{})});
}

bool RealtimeTouchInputRouter::beginLane(
    FingerState &finger, std::size_t laneIndex,
    const RealtimeTouchSample &sample) noexcept {
  if (laneIndex >= layout_.laneRegions.size()) {
    return false;
  }
  const auto &region = layout_.laneRegions[laneIndex];
  const int lane = region.lane;
  if (laneOccupied(lane, finger.fingerId)) {
    return false;
  }
  finger.lane = lane;
  finger.scratch = region.scratch;
  const auto replayControl = replay::logicalControlForChartLane(
      layout_.keyMode, lane, finger.scratch);
  finger.replayControl = replayControl;
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
  if (!emit(RealtimeGameplayInputType::Press, finger.lane, replayControl,
            sample.steadyTimestampMicros)) {
    return false;
  }
  finger.pressed = true;
  finger.scratchDirection = direction;
  finger.replayControl = replayControl;
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
    finger->lastX = sample.normalizedX;
    finger->lastY = sample.normalizedY;
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
      finger->lastX = sample.normalizedX;
      finger->lastY = sample.normalizedY;
      return true;
    }
    if (sample.excludedFromGameplay) {
      finger->lastX = sample.normalizedX;
      finger->lastY = sample.normalizedY;
      const bool released =
          releaseLane(*finger, sample.steadyTimestampMicros);
      finger->excluded = true;
      return released;
    }
    if (!layout_.dragMode) {
      if (finger->scratch) {
        return handleScratchMove(*finger, sample);
      }
      finger->lastX = sample.normalizedX;
      finger->lastY = sample.normalizedY;
      return true;
    }
    const auto lane = laneIndexAt(sample.normalizedX, sample.normalizedY, true);
    if (!lane.has_value()) {
      finger->lastX = sample.normalizedX;
      finger->lastY = sample.normalizedY;
      return releaseLane(*finger, sample.steadyTimestampMicros);
    }
    const int nextLane = layout_.laneRegions[*lane].lane;
    if (finger->lane == nextLane) {
      if (finger->scratch) {
        return handleScratchMove(*finger, sample);
      }
      finger->lastX = sample.normalizedX;
      finger->lastY = sample.normalizedY;
      return true;
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
    finger->lastX = sample.normalizedX;
    finger->lastY = sample.normalizedY;
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

bool RealtimeTouchInputRouter::setGameplayEnabled(
    bool enabled, std::int64_t steadyTimestampMicros) noexcept {
  if (gameplayEnabled_ == enabled) {
    return true;
  }
  const bool released = enabled || cancelAll(steadyTimestampMicros);
  gameplayEnabled_ = enabled;
  return released;
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
    if (sink_.cancelTouchLifecycle != nullptr) {
      success = sink_.cancelTouchLifecycle(
                    sink_.context,
                    {.fingerId = finger.fingerId,
                     .phase = RealtimeTouchPhase::Cancel,
                     .normalizedX = finger.lastX,
                     .normalizedY = finger.lastY,
                     .steadyTimestampMicros = steadyTimestampMicros,
                     .excludedFromGameplay = finger.excluded}) &&
                success;
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
  legacyUniformLayout_ = normalizeLayout(layout);
  layout_ = std::move(layout);
  return released;
}

} // namespace gameplay
