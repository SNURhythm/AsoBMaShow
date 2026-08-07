#include "VirtualControllerLayout.h"

#include "../../input/ChartLaneBinding.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gameplay {
namespace {

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }

[[nodiscard]] float clampOrigin(float desired, float minimum,
                                float maximum) noexcept {
  if (maximum < minimum) {
    return (minimum + maximum) * 0.5F;
  }
  return std::clamp(desired, minimum, maximum);
}

void extendBounds(VirtualControllerRect &bounds,
                  const VirtualControllerRect &element,
                  bool &initialized) noexcept {
  if (!initialized) {
    bounds = element;
    initialized = true;
    return;
  }
  const float right = std::max(bounds.x + bounds.width, element.x + element.width);
  const float bottom =
      std::max(bounds.y + bounds.height, element.y + element.height);
  bounds.x = std::min(bounds.x, element.x);
  bounds.y = std::min(bounds.y, element.y);
  bounds.width = right - bounds.x;
  bounds.height = bottom - bounds.y;
}

[[nodiscard]] std::optional<RealtimeTouchPoint>
normalizedPoint(float uiX, float uiY,
                const RealtimeTouchUiTransform &transform) noexcept {
  if (transform.renderWidth <= 0 || transform.renderHeight <= 0 ||
      !finite(transform.uiScaleX) || !finite(transform.uiScaleY) ||
      transform.uiScaleX <= 0.0F || transform.uiScaleY <= 0.0F) {
    return std::nullopt;
  }
  return RealtimeTouchPoint{
      .x = (uiX * transform.uiScaleX + static_cast<float>(transform.uiOffsetX)) /
           static_cast<float>(transform.renderWidth),
      .y = (uiY * transform.uiScaleY + static_cast<float>(transform.uiOffsetY)) /
           static_cast<float>(transform.renderHeight)};
}

} // namespace

bool VirtualControllerCanvas::valid() const noexcept {
  return finite(x) && finite(y) && finite(width) && finite(height) &&
         width > 0.0F && height > 0.0F;
}

bool VirtualControllerRect::valid() const noexcept {
  return finite(x) && finite(y) && finite(width) && finite(height) &&
         width > 0.0F && height > 0.0F;
}

bool VirtualControllerLayout::valid() const noexcept {
  return bounds.valid() && !elements.empty();
}

bool supportsVirtualControllerKeyMode(int keyMode) noexcept {
  return keyMode == 5 || keyMode == 7;
}

VirtualControllerLayout makeVirtualControllerLayout(
    const input::VirtualControllerConfig &config, int keyMode,
    VirtualControllerCanvas canvas) {
  VirtualControllerLayout layout;
  if (!config.enabled || !canvas.valid() ||
      !supportsVirtualControllerKeyMode(keyMode)) {
    return layout;
  }

  const float shortEdge = std::min(canvas.width, canvas.height);
  const float requestedUnit = shortEdge * config.buttonSize;
  const float requestedGap = requestedUnit * config.keyGap;
  if (!finite(requestedUnit) || !finite(requestedGap) || requestedUnit <= 0.0F ||
      requestedGap < 0.0F) {
    return layout;
  }

  // Keep every editable size usable. A wide 7-key group is constrained by
  // the current canvas rather than allowed to run off a small phone screen.
  const float requestedGridWidth =
      static_cast<float>(keyMode) * requestedUnit +
      static_cast<float>(keyMode - 1) * requestedGap;
  const float requestedGroupWidth = requestedUnit * 2.0F + requestedGap +
                                    requestedGridWidth;
  const float requestedGroupHeight = requestedUnit * 3.0F + requestedGap * 2.0F;
  const float scale = std::min(
      1.0F, std::min(canvas.width / requestedGroupWidth,
                      canvas.height / requestedGroupHeight));
  const float unit = requestedUnit * scale;
  const float gap = requestedGap * scale;
  if (!finite(unit) || !finite(gap) || unit <= 0.0F || gap < 0.0F) {
    return layout;
  }

  const float gridWidth =
      static_cast<float>(keyMode) * unit + static_cast<float>(keyMode - 1) * gap;
  const float groupWidth = unit * 2.0F + gap + gridWidth;
  const float systemHeight = unit * 0.55F;
  const float groupHeight = systemHeight + gap + unit * 2.0F + gap;
  const float desiredLeft = canvas.x + canvas.width * config.centerX - groupWidth * 0.5F;
  const float desiredTop = canvas.y + canvas.height * config.centerY - groupHeight * 0.5F;
  const float left = clampOrigin(desiredLeft, canvas.x,
                                 canvas.x + canvas.width - groupWidth);
  const float top = clampOrigin(desiredTop, canvas.y,
                                canvas.y + canvas.height - groupHeight);
  const float keyLeft = left + unit * 2.0F + gap;
  const float upperKeyTop = top + systemHeight + gap;
  const float lowerKeyTop = upperKeyTop + unit + gap;

  const float systemWidth = unit * 1.3F;
  const float systemGap = gap + unit * 0.35F;
  const float systemsWidth = systemWidth * 2.0F + systemGap;
  const float systemsLeft = left + (groupWidth - systemsWidth) * 0.5F;
  layout.elements.reserve(static_cast<std::size_t>(keyMode) + 3U);
  layout.elements.push_back(
      {.control = VirtualControllerControl::Start,
       .shape = VirtualControllerShape::Rectangle,
       .replayControl = replay::LogicalControl{
           .kind = replay::LogicalControlKind::Start, .player = 1, .lane = -1},
       .bounds = {.x = systemsLeft,
                  .y = top,
                  .width = systemWidth,
                  .height = systemHeight}});
  layout.elements.push_back(
      {.control = VirtualControllerControl::Select,
       .shape = VirtualControllerShape::Rectangle,
       .replayControl = replay::LogicalControl{
           .kind = replay::LogicalControlKind::Select, .player = 1, .lane = -1},
       .bounds = {.x = systemsLeft + systemWidth + systemGap,
                  .y = top,
                  .width = systemWidth,
                  .height = systemHeight}});

  bms_parser::ChartMeta meta;
  meta.KeyMode = keyMode;
  const auto scratchLanes = meta.GetScratchLaneIndices();
  if (scratchLanes.empty()) {
    return {};
  }
  layout.elements.push_back(
      {.control = VirtualControllerControl::Scratch,
       .shape = VirtualControllerShape::Circle,
       .lane = scratchLanes.front(),
       .scratch = true,
       .bounds = {.x = left,
                  .y = upperKeyTop + gap * 0.5F,
                  .width = unit * 2.0F,
                  .height = unit * 2.0F}});
  for (int keyPosition = 0; keyPosition < keyMode; ++keyPosition) {
    const auto lane = input_profile::chartLaneForKeyPosition(keyMode, keyPosition);
    if (!lane.has_value()) {
      return {};
    }
    layout.elements.push_back(
        {.control = VirtualControllerControl::Key,
         .shape = VirtualControllerShape::Rectangle,
         .keyPosition = keyPosition,
         .lane = *lane,
         .bounds = {.x = keyLeft + static_cast<float>(keyPosition) * (unit + gap),
                    .y = keyPosition % 2 == 0 ? lowerKeyTop : upperKeyTop,
                    .width = unit,
                    .height = unit}});
  }

  bool initialized = false;
  for (const auto &element : layout.elements) {
    extendBounds(layout.bounds, element.bounds, initialized);
  }
  return layout;
}

std::vector<RealtimeTouchLaneRegion>
makeVirtualControllerTouchRegions(const VirtualControllerLayout &layout,
                                  const RealtimeTouchUiTransform &transform) {
  std::vector<RealtimeTouchLaneRegion> regions;
  if (!layout.valid()) {
    return regions;
  }
  regions.reserve(layout.elements.size());
  for (const auto &element : layout.elements) {
    const auto bottomLeft = normalizedPoint(element.bounds.x,
                                            element.bounds.y + element.bounds.height,
                                            transform);
    const auto bottomRight = normalizedPoint(
        element.bounds.x + element.bounds.width,
        element.bounds.y + element.bounds.height, transform);
    const auto topLeft = normalizedPoint(element.bounds.x, element.bounds.y, transform);
    const auto topRight = normalizedPoint(element.bounds.x + element.bounds.width,
                                          element.bounds.y, transform);
    if (!bottomLeft || !bottomRight || !topLeft || !topRight) {
      return {};
    }
    RealtimeTouchLaneRegion region{.bottomLeft = *bottomLeft,
                                    .bottomRight = *bottomRight,
                                    .topLeft = *topLeft,
                                    .topRight = *topRight,
                                    .lane = element.lane,
                                    .scratch = element.scratch,
                                    .replayControl = element.replayControl,
                                    .requiresInside = true};
    if (element.shape == VirtualControllerShape::Circle) {
      const auto center = normalizedPoint(element.bounds.centerX(),
                                          element.bounds.centerY(), transform);
      const auto edge = normalizedPoint(
          element.bounds.centerX() + element.bounds.width * 0.5F,
          element.bounds.centerY(), transform);
      const auto verticalEdge = normalizedPoint(
          element.bounds.centerX(),
          element.bounds.centerY() + element.bounds.height * 0.5F, transform);
      if (!center || !edge || !verticalEdge) {
        return {};
      }
      region.circle = RealtimeTouchCircle{.center = *center,
                                          .radiusX = std::abs(edge->x - center->x),
                                          .radiusY = std::abs(verticalEdge->y - center->y)};
    }
    regions.push_back(std::move(region));
  }
  return regions;
}

} // namespace gameplay
