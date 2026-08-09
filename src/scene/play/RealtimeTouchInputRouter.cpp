#include "RealtimeTouchInputRouter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gameplay {
namespace {
constexpr std::int64_t kCancelledTouchGraceMicros = 50'000;
constexpr float kHitTestEpsilon = 0.000001F;
constexpr float kPi = 3.14159265358979323846F;
constexpr float kRadiansToDegrees = 180.0F / kPi;
// A virtual turntable has a stable three-degree angular tick. Its gameplay
// direction remains held through Beatoraja's pointer-scratch stop threshold,
// while Start/Select controls consume these angle ticks directly.
constexpr float kSpinScratchStepDegrees = 3.0F;
constexpr std::int64_t kSpinScratchGraceMicros = 150'000;

float shortestAngleDeltaRadians(float current, float previous) noexcept {
  float delta = std::fmod(current - previous, 2.0F * kPi);
  if (delta > kPi) {
    delta -= 2.0F * kPi;
  } else if (delta < -kPi) {
    delta += 2.0F * kPi;
  }
  return delta;
}

bool sameNonzeroSign(float left, float right) noexcept {
  return left != 0.0F && right != 0.0F &&
         std::signbit(left) == std::signbit(right);
}

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
  if (region.circle.has_value()) {
    const auto &circle = *region.circle;
    if (!isFinite(circle.center) || !std::isfinite(circle.radiusX) ||
        !std::isfinite(circle.radiusY) || circle.radiusX <= 0.0F ||
        circle.radiusY <= 0.0F) {
      return false;
    }
    const float dx = (point.x - circle.center.x) / circle.radiusX;
    const float dy = (point.y - circle.center.y) / circle.radiusY;
    return dx * dx + dy * dy <= 1.0F;
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

RealtimeTouchPoint center(const RealtimeTouchLaneRegion &region) noexcept {
  if (region.circle.has_value()) {
    return region.circle->center;
  }
  return RealtimeTouchPoint{(region.bottomLeft.x + region.bottomRight.x +
                             region.topLeft.x + region.topRight.x) *
                                0.25F,
                            (region.bottomLeft.y + region.bottomRight.y +
                             region.topLeft.y + region.topRight.y) *
                                0.25F};
}

float centerDistanceSq(const RealtimeTouchLaneRegion &region,
                      const RealtimeTouchPoint &point) noexcept {
  const auto pointCenter = center(region);
  const float dx = point.x - pointCenter.x;
  const float dy = point.y - pointCenter.y;
  return dx * dx + dy * dy;
}

bool contains(const PresentationUiHitRegion &region,
              UiLogicalPoint point) noexcept {
  if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
    return false;
  }
  if (region.circle.has_value()) {
    const auto &circle = *region.circle;
    if (!std::isfinite(circle.center.x) || !std::isfinite(circle.center.y) ||
        !std::isfinite(circle.radius) || circle.radius <= 0.0F) {
      return false;
    }
    const float dx = point.x - circle.center.x;
    const float dy = point.y - circle.center.y;
    return dx * dx + dy * dy <= circle.radius * circle.radius;
  }
  int winding = 0;
  for (std::size_t index = 0; index < region.boundary.size(); ++index) {
    const auto &from = region.boundary[index];
    const auto &to = region.boundary[(index + 1) % region.boundary.size()];
    if (!std::isfinite(from.x) || !std::isfinite(from.y) ||
        !std::isfinite(to.x) || !std::isfinite(to.y)) {
      return false;
    }
    const float side = (to.x - from.x) * (point.y - from.y) -
                       (to.y - from.y) * (point.x - from.x);
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

std::optional<UiLogicalPoint> RealtimeTouchHitSnapshot::uiPoint(
    float normalizedX, float normalizedY) const noexcept {
  if (!std::isfinite(normalizedX) || !std::isfinite(normalizedY) ||
      uiTransform.renderWidth <= 0 || uiTransform.renderHeight <= 0 ||
      !std::isfinite(uiTransform.uiScaleX) ||
      !std::isfinite(uiTransform.uiScaleY) || uiTransform.uiScaleX <= 0.0F ||
      uiTransform.uiScaleY <= 0.0F) {
    return std::nullopt;
  }
  return UiLogicalPoint{
      .x = (normalizedX * static_cast<float>(uiTransform.renderWidth) -
            static_cast<float>(uiTransform.uiOffsetX)) /
           uiTransform.uiScaleX,
      .y = (normalizedY * static_cast<float>(uiTransform.renderHeight) -
            static_cast<float>(uiTransform.uiOffsetY)) /
           uiTransform.uiScaleY};
}

std::optional<RealtimeTouchPoint> RealtimeTouchHitSnapshot::presentationPoint(
    float normalizedX, float normalizedY) const noexcept {
  const auto point = uiPoint(normalizedX, normalizedY);
  if (!point || uiTransform.uiWidth <= 0 || uiTransform.uiHeight <= 0) {
    return std::nullopt;
  }
  return RealtimeTouchPoint{
      .x = point->x / static_cast<float>(uiTransform.uiWidth),
      .y = point->y / static_cast<float>(uiTransform.uiHeight)};
}

PresentationUiHit RealtimeTouchHitSnapshot::hitTest(
    float normalizedX, float normalizedY) const noexcept {
  const auto point = uiPoint(normalizedX, normalizedY);
  if (!point) {
    return {};
  }
  for (const auto &region : regionsTopmostFirst) {
    if (region.hit.kind != PresentationUiControlKind::None &&
        contains(region, *point)) {
      return region.hit;
    }
  }
  return {};
}

bool RealtimeTouchHitSnapshotPublication::publish(
    RealtimeTouchHitSnapshot snapshot) noexcept {
  try {
    std::atomic_store_explicit(
        &published_,
        std::make_shared<const RealtimeTouchHitSnapshot>(std::move(snapshot)),
        std::memory_order_release);
    return true;
  } catch (...) {
    return false;
  }
}

std::shared_ptr<const RealtimeTouchHitSnapshot>
RealtimeTouchHitSnapshotPublication::acquire() const noexcept {
  return std::atomic_load_explicit(&published_, std::memory_order_acquire);
}

void RealtimeTouchHitSnapshotPublication::clear() noexcept {
  std::atomic_store_explicit(
      &published_, std::shared_ptr<const RealtimeTouchHitSnapshot>{},
      std::memory_order_release);
}

PresentationUiHit RealtimeTouchHitCaptureTracker::consume(
    const RealtimeTouchSample &sample,
    const RealtimeTouchHitSnapshot &snapshot) noexcept {
  auto *capture = [&]() -> Capture * {
    for (auto &candidate : captures_) {
      if (candidate.active && candidate.pointerId == sample.fingerId) {
        return &candidate;
      }
    }
    return nullptr;
  }();
  if (sample.phase == RealtimeTouchPhase::Down && capture == nullptr) {
    const auto hit = snapshot.hitTest(sample.normalizedX, sample.normalizedY);
    if (hit.kind == PresentationUiControlKind::None) {
      return {};
    }
    for (auto &candidate : captures_) {
      if (!candidate.active) {
        candidate = {
            .pointerId = sample.fingerId, .active = true, .hit = hit};
        capture = &candidate;
        break;
      }
    }
  }
  if (capture == nullptr) {
    return {};
  }
  const auto hit = capture->hit;
  if (sample.phase == RealtimeTouchPhase::Up ||
      sample.phase == RealtimeTouchPhase::Cancel) {
    *capture = {};
  }
  return hit;
}

void RealtimeTouchHitCaptureTracker::reset() noexcept {
  captures_.fill({});
}

void populateRealtimeTouchPresentationMetadata(
    RealtimeTouchSample &sample, const RealtimeTouchHitSnapshot &snapshot,
    RealtimeTouchHitCaptureTracker &captures) noexcept {
  sample.presentationHit = captures.consume(sample, snapshot);
  sample.presentationPoint =
      snapshot.presentationPoint(sample.normalizedX, sample.normalizedY);
  sample.presentationUiPoint.reset();
  if (sample.presentationHit.kind != PresentationUiControlKind::None) {
    sample.presentationUiPoint =
        snapshot.uiPoint(sample.normalizedX, sample.normalizedY);
  }
}

void RealtimeTouchPresentationDispatcher::setSink(
    RealtimeTouchPresentationSink sink) noexcept {
  captures_.fill({});
  sink_ = sink;
}

RealtimeTouchPresentationDispatcher::Capture *
RealtimeTouchPresentationDispatcher::find(std::int64_t pointerId) noexcept {
  for (auto &capture : captures_) {
    if (capture.active && capture.pointerId == pointerId) {
      return &capture;
    }
  }
  return nullptr;
}

RealtimeTouchPresentationDispatcher::Capture *
RealtimeTouchPresentationDispatcher::allocate(
    std::int64_t pointerId) noexcept {
  for (auto &capture : captures_) {
    if (!capture.active) {
      capture = {.pointerId = pointerId, .active = true};
      return &capture;
    }
  }
  return nullptr;
}

PresentationTouchResult RealtimeTouchPresentationDispatcher::consume(
    const RealtimeTouchSample &sample, long long eventMicros) noexcept {
  switch (sample.phase) {
  case RealtimeTouchPhase::Down: {
    if (sample.presentationHit.kind == PresentationUiControlKind::None ||
        sample.presentationHit.kind ==
            PresentationUiControlKind::NativeOverlay ||
        sample.presentationHit.kind ==
            PresentationUiControlKind::VirtualController ||
        !sample.presentationUiPoint ||
        find(sample.fingerId) != nullptr || sink_.begin == nullptr) {
      return {};
    }
    auto *capture = allocate(sample.fingerId);
    if (capture == nullptr) {
      return {};
    }
    capture->hit = sample.presentationHit;
    capture->uiPoint = *sample.presentationUiPoint;
    const PresentationTouchEvent event{.pointerId = sample.fingerId,
                                       .uiPoint = capture->uiPoint,
                                       .eventMicros = eventMicros,
                                       .hit = capture->hit};
    const auto result = sink_.begin(sink_.context, event);
    if (!result.consumed) {
      *capture = {};
    }
    return result;
  }
  case RealtimeTouchPhase::Move: {
    auto *capture = find(sample.fingerId);
    if (capture == nullptr || sink_.update == nullptr) {
      return {};
    }
    if (sample.presentationUiPoint) {
      capture->uiPoint = *sample.presentationUiPoint;
    }
    const PresentationTouchEvent event{.pointerId = sample.fingerId,
                                       .uiPoint = capture->uiPoint,
                                       .eventMicros = eventMicros,
                                       .hit = capture->hit};
    return sink_.update(sink_.context, event);
  }
  case RealtimeTouchPhase::Up:
  case RealtimeTouchPhase::Cancel: {
    auto *capture = find(sample.fingerId);
    if (capture == nullptr) {
      return {};
    }
    const PresentationTouchEvent event{.pointerId = sample.fingerId,
                                       .uiPoint = sample.presentationUiPoint
                                                      .value_or(capture->uiPoint),
                                       .eventMicros = eventMicros,
                                       .hit = capture->hit};
    *capture = {};
    return sink_.end != nullptr
               ? sink_.end(sink_.context, event,
                           sample.phase == RealtimeTouchPhase::Cancel)
               : PresentationTouchResult{};
  }
  case RealtimeTouchPhase::CancelExpired:
    return {};
  }
  return {};
}

void RealtimeTouchPresentationDispatcher::cancelAll(
    long long eventMicros) noexcept {
  bool hadCapture = false;
  for (auto &capture : captures_) {
    hadCapture = hadCapture || capture.active;
    capture = {};
  }
  if (hadCapture && sink_.cancelAll != nullptr) {
    sink_.cancelAll(sink_.context, eventMicros);
  }
}

void RealtimeTouchPresentationDispatcher::reconcileMetadataOverflow(
    long long eventMicros) noexcept {
  cancelAll(eventMicros);
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
  std::optional<std::size_t> firstAuthoredSkin;
  std::optional<std::size_t> nearestVirtualControl;
  float nearestVirtualControlDistanceSq = 0.0F;
  for (std::size_t index = 0; index < layout_.laneRegions.size(); ++index) {
    const auto &region = layout_.laneRegions[index];
    if (!contains(region, point)) {
      continue;
    }
    if (!region.requiresInside) {
      if (!firstAuthoredSkin.has_value()) {
        firstAuthoredSkin = index;
      }
      continue;
    }
    const float distanceSq = centerDistanceSq(region, point);
    if (!nearestVirtualControl.has_value() ||
        distanceSq < nearestVirtualControlDistanceSq) {
      nearestVirtualControl = index;
      nearestVirtualControlDistanceSq = distanceSq;
    }
  }
  if (nearestVirtualControl.has_value()) {
    return nearestVirtualControl;
  }
  if (firstAuthoredSkin.has_value()) {
    return firstAuthoredSkin;
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
    if (layout_.laneRegions[index].requiresInside) {
      continue;
    }
    const auto clamped = clampedVertically(layout_.laneRegions[index], point);
    if (contains(layout_.laneRegions[index], clamped)) {
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
  if (lane < 0) {
    return false;
  }
  return std::ranges::any_of(fingers_, [&](const FingerState &finger) {
    return finger.active && finger.fingerId != exceptFinger &&
           finger.lane == lane;
  });
}

bool RealtimeTouchInputRouter::emit(RealtimeGameplayInputType type, int lane,
                                    std::optional<replay::LogicalControl>
                                        replayControl,
                                    std::int64_t timestampMicros,
                                    bool backSpin,
                                    bool analogScratch) noexcept {
  return sink_.emit != nullptr &&
         sink_.emit(sink_.context,
                    {.epoch = epoch_,
                     .type = type,
                     .source = RealtimeGameplayInputSource::Touch,
                     .lane = lane,
                     .compensateLane = lane,
                     .backSpin = backSpin,
                     .analogScratch = analogScratch,
                     .steadyTimestampMicros = timestampMicros,
                     .hasReplayControl = replayControl.has_value(),
                     .replayControl = replayControl.value_or(
                         replay::LogicalControl{})});
}

void RealtimeTouchInputRouter::emitAnalogScratchTicks(
    replay::LogicalControl control, int ticks,
    std::int64_t timestampMicros) noexcept {
  if (ticks > 0 && sink_.emitAnalogScratchTicks != nullptr) {
    sink_.emitAnalogScratchTicks(sink_.context, control, ticks,
                                 timestampMicros);
  }
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
  finger.spinScratch = region.scratch && region.spinScratch &&
                       region.circle.has_value();
  finger.invertFlickScratchDirection = region.scratch && !finger.spinScratch &&
                                       region.invertFlickScratchDirection;
  const auto replayControl = region.replayControl.has_value()
                                 ? region.replayControl
                                 : replay::logicalControlForChartLane(
                                       layout_.keyMode, lane, finger.scratch);
  finger.replayControl = replayControl;
  finger.pressed = false;
  finger.scratchDirection = 0;
  finger.lastX = sample.normalizedX;
  finger.lastY = sample.normalizedY;
  if (finger.spinScratch) {
    finger.spinCenter = region.circle->center;
    finger.spinPreviousAngleRadians = std::atan2(
        sample.normalizedY - finger.spinCenter.y,
        sample.normalizedX - finger.spinCenter.x);
  }
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
  const bool shouldEmit = finger.pressed;
  const auto replayControl = finger.replayControl;
  if (shouldEmit &&
      !emit(RealtimeGameplayInputType::Release, lane, replayControl,
            timestampMicros, backSpin, finger.spinScratch)) {
    return false;
  }
  finger.lane = -1;
  finger.pressed = false;
  finger.scratch = false;
  finger.spinScratch = false;
  finger.scratchDirection = 0;
  finger.spinAccumulatedDegrees = 0.0F;
  finger.spinLastStepMicros = 0;
  finger.cancelDeadlineMicros = 0;
  return true;
}

bool RealtimeTouchInputRouter::handleScratchMove(
    FingerState &finger, const RealtimeTouchSample &sample) noexcept {
  if (finger.spinScratch) {
    return handleSpinScratchMove(finger, sample);
  }
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
  int direction = dy < 0.0F ? 1 : -1;
  if (finger.invertFlickScratchDirection) {
    direction = -direction;
  }
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

bool RealtimeTouchInputRouter::handleSpinScratchMove(
    FingerState &finger, const RealtimeTouchSample &sample) noexcept {
  const float currentAngle = std::atan2(sample.normalizedY - finger.spinCenter.y,
                                        sample.normalizedX - finger.spinCenter.x);
  if (!std::isfinite(currentAngle)) {
    return true;
  }
  const float deltaRadians = shortestAngleDeltaRadians(
      currentAngle, finger.spinPreviousAngleRadians);
  finger.spinPreviousAngleRadians = currentAngle;
  finger.lastX = sample.normalizedX;
  finger.lastY = sample.normalizedY;
  if (!std::isfinite(deltaRadians)) {
    return true;
  }
  const float deltaDegrees = deltaRadians * kRadiansToDegrees;
  spinScratchRotationDegrees_ += deltaDegrees;
  if (deltaDegrees == 0.0F) {
    return true;
  }
  if (finger.spinAccumulatedDegrees != 0.0F &&
      !sameNonzeroSign(finger.spinAccumulatedDegrees, deltaDegrees)) {
    finger.spinAccumulatedDegrees = 0.0F;
  }
  finger.spinAccumulatedDegrees += deltaDegrees;
  const int completedTicks = static_cast<int>(std::trunc(
      finger.spinAccumulatedDegrees / kSpinScratchStepDegrees));
  if (completedTicks == 0) {
    return true;
  }
  finger.spinAccumulatedDegrees -=
      static_cast<float>(completedTicks) * kSpinScratchStepDegrees;
  finger.spinLastStepMicros = sample.steadyTimestampMicros;
  const int direction = completedTicks > 0 ? 1 : -1;
  const auto replayControl = replay::logicalControlForChartLane(
      layout_.keyMode, finger.lane, true,
      direction > 0 ? replay::LogicalControlKind::ScratchClockwise
                    : replay::LogicalControlKind::ScratchCounterClockwise);
  if (finger.pressed && direction == finger.scratchDirection) {
    if (replayControl.has_value()) {
      emitAnalogScratchTicks(*replayControl, std::abs(completedTicks),
                             sample.steadyTimestampMicros);
    }
    return true;
  }
  if (finger.pressed &&
      !emit(RealtimeGameplayInputType::Release, finger.lane,
            finger.replayControl, sample.steadyTimestampMicros, true, true)) {
    return false;
  }
  finger.pressed = false;
  if (!emit(RealtimeGameplayInputType::Press, finger.lane, replayControl,
            sample.steadyTimestampMicros, false, true)) {
    return false;
  }
  finger.pressed = true;
  finger.scratchDirection = direction;
  finger.replayControl = replayControl;
  if (replayControl.has_value()) {
    emitAnalogScratchTicks(*replayControl, std::abs(completedTicks),
                           sample.steadyTimestampMicros);
  }
  return true;
}

bool RealtimeTouchInputRouter::consume(
    const RealtimeTouchSample &sample) noexcept {
  return consumeForPublication(sample) !=
         RealtimeTouchRoutingDisposition::RetryRequired;
}

RealtimeTouchRoutingDisposition
RealtimeTouchInputRouter::consumeForPublication(
    const RealtimeTouchSample &sample) noexcept {
  bool publishAuxiliary = true;
  if (!consumeImpl(sample, publishAuxiliary)) {
    return RealtimeTouchRoutingDisposition::RetryRequired;
  }
  return publishAuxiliary ? RealtimeTouchRoutingDisposition::Accepted
                          : RealtimeTouchRoutingDisposition::Inert;
}

bool RealtimeTouchInputRouter::consumeImpl(
    const RealtimeTouchSample &sample, bool &publishAuxiliary) noexcept {
  publishAuxiliary = true;
  if (!gameplayEnabled_) {
    return true;
  }
  switch (sample.phase) {
  case RealtimeTouchPhase::Down: {
    auto *finger = findFinger(sample.fingerId);
    if (finger != nullptr && finger->suppressedUntilLift) {
      *finger = {.fingerId = sample.fingerId, .active = true};
    } else if (finger != nullptr && finger->cancelDeadlineMicros != 0) {
      // A new Down with the same ID during the cancel grace period is a new
      // physical contact, not a continuation. Release the prior contact
      // first; on failure leave it untouched so this Down can retry.
      if (!finger->excluded &&
          !releaseLane(*finger, sample.steadyTimestampMicros)) {
        return false;
      }
      *finger = {.fingerId = sample.fingerId, .active = true};
    } else if (finger != nullptr) {
      // Native backends can duplicate Down. The original contact remains the
      // sole owner until a continuation, cancellation, or lift changes it.
      publishAuxiliary = false;
      return true;
    } else {
      finger = allocateFinger(sample.fingerId);
    }
    if (finger == nullptr) {
      return false;
    }
    finger->lastX = sample.normalizedX;
    finger->lastY = sample.normalizedY;
    finger->presentationHit = sample.presentationHit;
    finger->presentationUiPoint = sample.presentationUiPoint;
    finger->presentationPoint = sample.presentationPoint;
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
    if (*lane < layout_.laneRegions.size() &&
        laneOccupied(layout_.laneRegions[*lane].lane, finger->fingerId)) {
      finger->active = false;
      publishAuxiliary = false;
      return true;
    }
    if (!beginLane(*finger, *lane, sample)) {
      // A failed worker Press was never accepted, but the physical contact is
      // still down. Retain a tombstone so a stale Move after fail-closed
      // recovery cannot enter drag routing without a new Down.
      finger->lane = -1;
      finger->pressed = false;
      finger->excluded = true;
      finger->suppressedUntilLift = true;
      finger->active = true;
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
    if (finger->suppressedUntilLift) {
      finger->lastX = sample.normalizedX;
      finger->lastY = sample.normalizedY;
      publishAuxiliary = false;
      return true;
    }
    finger->cancelDeadlineMicros = 0;
    if (finger->excluded) {
      finger->lastX = sample.normalizedX;
      finger->lastY = sample.normalizedY;
      if (sample.presentationUiPoint) {
        finger->presentationUiPoint = sample.presentationUiPoint;
      }
      if (sample.presentationPoint) {
        finger->presentationPoint = sample.presentationPoint;
      }
      return true;
    }
    if (sample.excludedFromGameplay) {
      finger->lastX = sample.normalizedX;
      finger->lastY = sample.normalizedY;
      const bool released =
          releaseLane(*finger, sample.steadyTimestampMicros);
      if (released) {
        finger->excluded = true;
        finger->presentationHit = sample.presentationHit;
        finger->presentationUiPoint = sample.presentationUiPoint;
        finger->presentationPoint = sample.presentationPoint;
      }
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
    const auto &nextRegion = layout_.laneRegions[*lane];
    const int nextLane = nextRegion.lane;
    const auto nextReplayControl =
        nextRegion.replayControl.has_value()
            ? nextRegion.replayControl
            : replay::logicalControlForChartLane(layout_.keyMode, nextLane,
                                                 nextRegion.scratch);
    if (finger->lane == nextLane && finger->scratch == nextRegion.scratch &&
        finger->replayControl == nextReplayControl) {
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
    if (finger->suppressedUntilLift) {
      *finger = {};
      publishAuxiliary = false;
      return true;
    }
    const bool released =
        finger->excluded
            ? true
            : releaseLane(*finger, sample.steadyTimestampMicros);
    if (released) {
      finger->active = false;
    }
    return released;
  }
  case RealtimeTouchPhase::Cancel: {
    auto *finger = findFinger(sample.fingerId);
    if (finger == nullptr) {
      return true;
    }
    if (finger->suppressedUntilLift) {
      publishAuxiliary = false;
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
    // This phase is generated by the router's grace timer, not by the native
    // stream, and must never appear in presentation/replay publication.
    publishAuxiliary = false;
    auto *finger = findFinger(sample.fingerId);
    if (finger != nullptr && finger->suppressedUntilLift) {
      *finger = {};
      return true;
    }
    if (finger == nullptr || finger->cancelDeadlineMicros == 0 ||
        sample.steadyTimestampMicros < finger->cancelDeadlineMicros) {
      return true;
    }
    const bool released =
        finger->excluded
            ? true
            : releaseLane(*finger, sample.steadyTimestampMicros);
    if (released) {
      finger->cancelDeadlineMicros = 0;
      finger->active = false;
    }
    return released;
  }
  }
  return false;
}

bool RealtimeTouchInputRouter::acknowledgePublishedCancellation(
    std::int64_t fingerId) noexcept {
  auto *finger = findFinger(fingerId);
  if (finger == nullptr || finger->suppressedUntilLift ||
      finger->cancelDeadlineMicros == 0) {
    return false;
  }
  finger->cancellationPublished = true;
  return true;
}

bool RealtimeTouchInputRouter::advanceSpinScratch(
    std::int64_t steadyTimestampMicros) noexcept {
  for (auto &finger : fingers_) {
    if (!finger.active || !finger.spinScratch || !finger.pressed ||
        finger.spinLastStepMicros <= 0 ||
        steadyTimestampMicros < finger.spinLastStepMicros ||
        steadyTimestampMicros - finger.spinLastStepMicros <
            kSpinScratchGraceMicros) {
      continue;
    }
    if (!emit(RealtimeGameplayInputType::Release, finger.lane,
              finger.replayControl, steadyTimestampMicros, false, true)) {
      return false;
    }
    // Keep the physical contact captured so a later turn starts a fresh
    // direction without requiring another finger-down.
    finger.pressed = false;
    finger.scratchDirection = 0;
    finger.spinLastStepMicros = 0;
  }
  return true;
}

float RealtimeTouchInputRouter::spinScratchRotationDegrees() const noexcept {
  return spinScratchRotationDegrees_;
}

bool RealtimeTouchInputRouter::setGameplayEnabled(
    bool enabled, std::int64_t steadyTimestampMicros) noexcept {
  if (!enabled) {
    gameplayEnabled_ = false;
    return cancelAll(steadyTimestampMicros);
  }
  if (!cancelAll(steadyTimestampMicros)) {
    gameplayEnabled_ = false;
    return false;
  }
  gameplayEnabled_ = true;
  return true;
}

void RealtimeTouchInputRouter::reset() noexcept {
  for (auto &finger : fingers_) {
    finger = {};
  }
  spinScratchRotationDegrees_ = 0.0F;
}

bool RealtimeTouchInputRouter::cancelAll(
    std::int64_t steadyTimestampMicros) noexcept {
  bool success = true;
  for (auto &finger : fingers_) {
    if (!finger.active) {
      continue;
    }
    if (finger.suppressedUntilLift) {
      continue;
    }
    if (!finger.cancellationPublished &&
        sink_.cancelTouchLifecycle != nullptr) {
      if (!sink_.cancelTouchLifecycle(
              sink_.context,
              {.fingerId = finger.fingerId,
               .phase = RealtimeTouchPhase::Cancel,
               .normalizedX = finger.lastX,
               .normalizedY = finger.lastY,
               .steadyTimestampMicros = steadyTimestampMicros,
               .excludedFromGameplay = finger.excluded,
               .presentationHit = finger.presentationHit,
               .presentationUiPoint = finger.presentationUiPoint,
               .presentationPoint = finger.presentationPoint})) {
        success = false;
        continue;
      }
      finger.cancellationPublished = true;
    }
    if (!releaseLane(finger, steadyTimestampMicros)) {
      success = false;
      continue;
    }
    finger.excluded = true;
    finger.suppressedUntilLift = true;
  }
  return success;
}

bool RealtimeTouchInputRouter::updateLayout(
    RealtimeTouchLayout layout,
    std::int64_t steadyTimestampMicros) noexcept {
  const bool released = cancelAll(steadyTimestampMicros);
  if (!released) {
    return false;
  }
  legacyUniformLayout_ = normalizeLayout(layout);
  layout_ = std::move(layout);
  return true;
}

} // namespace gameplay
