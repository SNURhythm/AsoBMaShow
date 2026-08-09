#include "VirtualControllerLayout.h"

#include "../../input/ChartLaneBinding.h"

#include <algorithm>
#include <cmath>

namespace gameplay {
namespace {

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }

[[nodiscard]] int keysPerVirtualControllerPlayer(int keyMode) noexcept {
  switch (keyMode) {
  case 5:
  case 10:
    return 5;
  case 7:
  case 14:
    return 7;
  default:
    return 0;
  }
}

[[nodiscard]] bool isDoublePlayKeyMode(int keyMode) noexcept {
  return keyMode == 10 || keyMode == 14;
}

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
  return keysPerVirtualControllerPlayer(keyMode) != 0;
}

VirtualControllerLayout makeVirtualControllerLayout(
    const input::VirtualControllerConfig &config, int keyMode,
    VirtualControllerCanvas canvas) {
  VirtualControllerLayout layout;
  if (!config.enabled || !canvas.valid() ||
      !supportsVirtualControllerKeyMode(keyMode)) {
    return layout;
  }

  if (!finite(config.centerX) || !finite(config.centerY) ||
      !finite(config.buttonSize) || !finite(config.keySpacingX) ||
      !finite(config.keySpacingY) ||
      !finite(config.scratchKeyplateSpacing)) {
    return layout;
  }
  const float shortEdge = std::min(canvas.width, canvas.height);
  const float requestedUnit = shortEdge * config.buttonSize;
  if (!finite(requestedUnit) || requestedUnit <= 0.0F) {
    return layout;
  }

  bms_parser::ChartMeta meta;
  meta.KeyMode = keyMode;
  const int keysPerPlayer = keysPerVirtualControllerPlayer(keyMode);
  const bool drawPlayerTwo =
      config.player == input::VirtualControllerPlayer::Player2;
  const int chartPlayer = drawPlayerTwo && isDoublePlayKeyMode(keyMode) ? 2 : 1;
  const auto scratchLanes = meta.GetScratchLaneIndices();
  const auto allKeyLanes = meta.GetKeyLaneIndices();
  const std::size_t keyOffset =
      chartPlayer == 2 ? static_cast<std::size_t>(keysPerPlayer) : 0U;
  const std::size_t scratchOffset = chartPlayer == 2 ? 1U : 0U;
  if (keysPerPlayer <= 0 || scratchOffset >= scratchLanes.size() ||
      keyOffset + static_cast<std::size_t>(keysPerPlayer) >
          allKeyLanes.size()) {
    return {};
  }
  std::vector<int> keyLanes;
  keyLanes.reserve(static_cast<std::size_t>(keysPerPlayer));
  for (int keyPosition = 0; keyPosition < keysPerPlayer; ++keyPosition) {
    keyLanes.push_back(
        allKeyLanes[keyOffset + static_cast<std::size_t>(keyPosition)]);
  }

  const auto makeElements = [&](float unit) {
    std::vector<VirtualControllerElement> elements;
    const float keyWidth = unit;
    const float keyHeight = unit * 2.0F;
    const float keyPitchX =
        keyWidth * (1.0F + config.keySpacingX);
    const float keyPitchY =
        keyHeight * (1.0F + config.keySpacingY);
    const float scratchDiameter = keyHeight * 1.8F;
    const float scratchToKeyplateGap =
        keyWidth * config.scratchKeyplateSpacing;
    const float systemSize = keyWidth;
    const float systemGap = keyWidth * 0.5F;
    if (!finite(keyPitchX) || !finite(keyPitchY) ||
        !finite(scratchToKeyplateGap) || keyPitchX <= 0.0F ||
        keyPitchY <= 0.0F) {
      return elements;
    }

    const float keyplateLeft = scratchDiameter + scratchToKeyplateGap;
    const float upperKeyTop = systemSize + keyHeight * 0.25F;
    const float lowerKeyTop = upperKeyTop + keyPitchY;
    const float keyplateRight =
        keyplateLeft + static_cast<float>(keysPerPlayer - 1) * keyPitchX +
        keyWidth;
    const float systemsLeft =
        (keyplateLeft + keyplateRight) * 0.5F - (systemSize * 2.0F + systemGap) * 0.5F;
    const float scratchTop = upperKeyTop +
                             (keyPitchY + keyHeight - scratchDiameter) * 0.5F;

    elements.reserve(static_cast<std::size_t>(keysPerPlayer) + 3U);
    elements.push_back(
        {.control = VirtualControllerControl::Start,
         .shape = VirtualControllerShape::Rectangle,
         .replayControl =
             replay::LogicalControl{.kind = replay::LogicalControlKind::Start,
                                    .player = chartPlayer,
                                    .lane = -1},
         .bounds = {.x = systemsLeft,
                    .y = 0.0F,
                    .width = systemSize,
                    .height = systemSize}});
    elements.push_back(
        {.control = VirtualControllerControl::Select,
         .shape = VirtualControllerShape::Rectangle,
         .replayControl =
             replay::LogicalControl{.kind = replay::LogicalControlKind::Select,
                                    .player = chartPlayer,
                                    .lane = -1},
         .bounds = {.x = systemsLeft + systemSize + systemGap,
                    .y = 0.0F,
                    .width = systemSize,
                    .height = systemSize}});
    elements.push_back(
        {.control = VirtualControllerControl::Scratch,
         .shape = VirtualControllerShape::Circle,
         .lane = scratchLanes[scratchOffset],
         .scratch = true,
         .spinScratch = config.scratchMode ==
                        input::VirtualControllerScratchMode::Spin,
         .invertFlickScratchDirection =
             config.scratchMode == input::VirtualControllerScratchMode::Flick &&
             !drawPlayerTwo,
         .bounds = {.x = 0.0F,
                    .y = scratchTop,
                    .width = scratchDiameter,
                    .height = scratchDiameter}});
    for (int keyPosition = 0; keyPosition < keysPerPlayer; ++keyPosition) {
      elements.push_back(
          {.control = VirtualControllerControl::Key,
           .shape = VirtualControllerShape::Rectangle,
           .keyPosition = keyPosition,
           .lane = keyLanes[static_cast<std::size_t>(keyPosition)],
           .bounds = {.x = keyplateLeft +
                          static_cast<float>(keyPosition) * keyPitchX,
                      .y = keyPosition % 2 == 0 ? lowerKeyTop : upperKeyTop,
                      .width = keyWidth,
                      .height = keyHeight}});
    }
    if (drawPlayerTwo) {
      // 2P uses the second deck's lanes but retains the same visual key and
      // system-button order. Only the platter moves to the opposite side;
      // reflecting the complete layout would put key position 0 on the right.
      const auto scratch = std::ranges::find_if(
          elements, [](const auto &element) {
            return element.control == VirtualControllerControl::Scratch;
          });
      if (scratch != elements.end()) {
        scratch->bounds.x = keyplateRight + scratchToKeyplateGap;
      }
    }
    return elements;
  };

  const auto elementsBounds = [](const auto &elements) {
    VirtualControllerRect bounds;
    bool initialized = false;
    for (const auto &element : elements) {
      extendBounds(bounds, element.bounds, initialized);
    }
    return initialized ? bounds : VirtualControllerRect{};
  };
  const auto requestedElements = makeElements(requestedUnit);
  const auto requestedBounds = elementsBounds(requestedElements);
  if (!requestedBounds.valid()) {
    return {};
  }
  const float scale = std::min(
      1.0F, std::min(canvas.width / requestedBounds.width,
                      canvas.height / requestedBounds.height));
  if (!finite(scale) || scale <= 0.0F) {
    return {};
  }
  layout.elements = makeElements(requestedUnit * scale);
  const auto rawBounds = elementsBounds(layout.elements);
  if (!rawBounds.valid()) {
    return {};
  }
  const float desiredOffsetX = canvas.x + canvas.width * config.centerX -
                               rawBounds.centerX();
  const float desiredOffsetY = canvas.y + canvas.height * config.centerY -
                               rawBounds.centerY();
  const float offsetX = clampOrigin(desiredOffsetX, canvas.x - rawBounds.x,
                                    canvas.x + canvas.width -
                                        (rawBounds.x + rawBounds.width));
  const float offsetY = clampOrigin(desiredOffsetY, canvas.y - rawBounds.y,
                                    canvas.y + canvas.height -
                                        (rawBounds.y + rawBounds.height));
  bool initialized = false;
  for (auto &element : layout.elements) {
    element.bounds.x += offsetX;
    element.bounds.y += offsetY;
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
  constexpr float kTouchRegionExtra = 0.5F;
  regions.reserve(layout.elements.size());
  for (const auto &element : layout.elements) {
    const float centerX = element.bounds.centerX();
    const float centerY = element.bounds.centerY();
    const float uniformExtra =
        0.5F * std::min(element.bounds.width, element.bounds.height) *
        kTouchRegionExtra;
    const float halfWidth = element.bounds.width * 0.5F + uniformExtra;
    const float halfHeight = element.bounds.height * 0.5F + uniformExtra;
    const auto expandedLeft = centerX - halfWidth;
    const auto expandedRight = centerX + halfWidth;
    const auto expandedBottom = centerY + halfHeight;
    const auto expandedTop = centerY - halfHeight;
    const auto bottomLeft = normalizedPoint(expandedLeft, expandedBottom, transform);
    const auto bottomRight =
        normalizedPoint(expandedRight, expandedBottom, transform);
    const auto topLeft = normalizedPoint(expandedLeft, expandedTop, transform);
    const auto topRight =
        normalizedPoint(expandedRight, expandedTop, transform);
    if (!bottomLeft || !bottomRight || !topLeft || !topRight) {
      return {};
    }
    RealtimeTouchLaneRegion region{.bottomLeft = *bottomLeft,
                                   .bottomRight = *bottomRight,
                                   .topLeft = *topLeft,
                                   .topRight = *topRight,
                                   .lane = element.lane,
                                   .scratch = element.scratch,
                                   .spinScratch = element.spinScratch,
                                   .invertFlickScratchDirection =
                                       element.invertFlickScratchDirection,
                                   .replayControl = element.replayControl,
                                   .requiresInside = true};
    if (element.shape == VirtualControllerShape::Circle) {
      const auto center = normalizedPoint(centerX, centerY, transform);
      const auto edge = normalizedPoint(centerX + halfWidth, centerY, transform);
      const auto verticalEdge = normalizedPoint(
          centerX, centerY + halfHeight, transform);
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
