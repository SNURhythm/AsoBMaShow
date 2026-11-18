#include "VirtualControllerLayout.h"

#include "../../input/ChartLaneBinding.h"

#include <algorithm>
#include <cmath>

namespace gameplay {
namespace {

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }

[[nodiscard]] int keysPerVirtualControllerPlayer(int keyMode) noexcept {
  switch (input_profile::canonicalChartKeyMode(keyMode)) {
  case 4:
  case 6:
  case 8:
    return keyMode;
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

RealtimeTouchPoint rotateControllerOffset(float x, float y, float degrees) noexcept {
  if (degrees == 0) return {x, y};
  const float radians = degrees * (3.14159265359F / 180.0F);
  const float c = std::cos(radians), s = std::sin(radians);
  return {c * x - s * y, s * x + c * y};
}

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
  meta.KeyMode = input_profile::canonicalChartKeyMode(keyMode);
  const int keysPerPlayer = keysPerVirtualControllerPlayer(keyMode);
  const bool drawPlayerTwo =
      config.player == input::VirtualControllerPlayer::Player2;
  const int chartPlayer = drawPlayerTwo && isDoublePlayKeyMode(keyMode) ? 2 : 1;
  const bool commandOnlyScratch = input_profile::usesCommandOnlyScratch(keyMode);
  const auto scratchLanes = commandOnlyScratch ? std::vector<int>{-1}
                                             : meta.GetScratchLaneIndices();
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

    const bool staggered = keysPerPlayer % 2 != 0;
    const float keyplateLeft = config.scratchEnabled
                                  ? scratchDiameter + scratchToKeyplateGap : 0.0F;
    const float upperKeyTop = systemSize + keyHeight * 0.25F;
    const float lowerKeyTop = upperKeyTop + (staggered ? keyPitchY : 0.0F);
    const float keyplateRight =
        keyplateLeft + static_cast<float>(keysPerPlayer - 1) * keyPitchX +
        keyWidth;
    const float systemsLeft =
        (keyplateLeft + keyplateRight) * 0.5F - (systemSize * 2.0F + systemGap) * 0.5F;
    const float scratchTop = upperKeyTop +
                             ((staggered ? keyPitchY : 0.0F) + keyHeight - scratchDiameter) * 0.5F;

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
    if (config.scratchEnabled) {
      elements.push_back(
          {.control = VirtualControllerControl::Scratch,
           .shape = VirtualControllerShape::Circle,
           .lane = scratchLanes[scratchOffset],
           .scratch = true,
           .spinScratch = config.scratchMode ==
                          input::VirtualControllerScratchMode::Spin,
           .touchScratch = config.scratchMode == input::VirtualControllerScratchMode::Touch,
           .invertFlickScratchDirection =
               config.scratchMode == input::VirtualControllerScratchMode::Flick &&
               !drawPlayerTwo,
           .replayControl = commandOnlyScratch
               ? std::optional(replay::LogicalControl{
                     .kind = replay::LogicalControlKind::ScratchClockwise,
                     .player = chartPlayer})
               : std::nullopt,
           .bounds = {.x = 0.0F,
                      .y = scratchTop,
                      .width = scratchDiameter,
                      .height = scratchDiameter}});
    }
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
  if (!finite(config.plateRotationDegrees)) return {};
  const auto plateAxis = rotateControllerOffset(1, 0, config.plateRotationDegrees);
  const float c = std::abs(plateAxis.x), s = std::abs(plateAxis.y);
  const float plateWidth = c * requestedBounds.width + s * requestedBounds.height;
  const float plateHeight = s * requestedBounds.width + c * requestedBounds.height;
  const float scale = std::min({1.0F, canvas.width / plateWidth, canvas.height / plateHeight});
  if (!finite(scale) || scale <= 0.0F) return {};
  layout.elements = makeElements(requestedUnit * scale);
  const auto rawBounds = elementsBounds(layout.elements);
  const float halfWidth = plateWidth * scale * 0.5F;
  const float halfHeight = plateHeight * scale * 0.5F;
  const float pivotX = clampOrigin(canvas.x + canvas.width * config.centerX,
      canvas.x + halfWidth, canvas.x + canvas.width - halfWidth);
  const float pivotY = clampOrigin(canvas.y + canvas.height * config.centerY,
      canvas.y + halfHeight, canvas.y + canvas.height - halfHeight);
  layout.unit = requestedUnit * scale;
  layout.touchCompensation = std::isfinite(config.touchCompensation)
                                ? std::clamp(config.touchCompensation, 0.0F, 2.0F) : 0.5F;
  std::erase_if(layout.elements, [&](const auto &element) {
    return (element.control == VirtualControllerControl::Start && !config.startEnabled) ||
           (element.control == VirtualControllerControl::Select && !config.selectEnabled);
  });
  bool initialized = false;
  for (auto &element : layout.elements) {
    const auto &custom = config.controls[element.configIndex()];
    if (!finite(custom.sizeScale) || custom.sizeScale <= 0.0F ||
        !finite(custom.offsetX) || !finite(custom.offsetY)) return {};
    auto &bounds = element.bounds;
    if (!finite(custom.rotationDegrees)) return {};
    const auto center = rotateControllerOffset(bounds.centerX() - rawBounds.centerX(),
        bounds.centerY() - rawBounds.centerY(), config.plateRotationDegrees);
    const auto offset = rotateControllerOffset(custom.offsetX * layout.unit,
        custom.offsetY * layout.unit, config.plateRotationDegrees);
    element.rotationDegrees = config.plateRotationDegrees + custom.rotationDegrees;
    const auto axis = rotateControllerOffset(1, 0,
        element.scratch ? 0 : element.rotationDegrees);
    float width = std::abs(axis.x) * bounds.width + std::abs(axis.y) * bounds.height;
    float height = std::abs(axis.y) * bounds.width + std::abs(axis.x) * bounds.height;
    const float fit = std::min({custom.sizeScale, canvas.width / width, canvas.height / height});
    bounds.width *= fit;
    bounds.height *= fit;
    width *= fit;
    height *= fit;
    const float cx = clampOrigin(pivotX + center.x + offset.x, canvas.x + width / 2,
                                 canvas.x + canvas.width - width / 2);
    const float cy = clampOrigin(pivotY + center.y + offset.y, canvas.y + height / 2,
                                 canvas.y + canvas.height - height / 2);
    bounds.x = cx - bounds.width / 2;
    bounds.y = cy - bounds.height / 2;
    extendBounds(layout.bounds, {cx - width / 2, cy - height / 2, width, height}, initialized);
  }
  return layout;
}

const VirtualControllerLayout &VirtualControllerLayoutCache::get(
    const input::VirtualControllerConfig &config, int keyMode,
    VirtualControllerCanvas canvas) {
  if (initialized_ && config_ == config && keyMode_ == keyMode &&
      canvas_ == canvas) {
    return layout_;
  }
  layout_ = makeVirtualControllerLayout(config, keyMode, canvas);
  config_ = config;
  keyMode_ = keyMode;
  canvas_ = canvas;
  initialized_ = true;
  return layout_;
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
    const float centerX = element.bounds.centerX();
    const float centerY = element.bounds.centerY();
    const float uniformExtra =
        0.5F * std::min(element.bounds.width, element.bounds.height) *
        layout.touchCompensation;
    const float halfWidth = element.bounds.width * 0.5F + uniformExtra;
    const float halfHeight = element.bounds.height * 0.5F + uniformExtra;
    const auto corner = [&](float dx, float dy) {
      const auto point = element.pointAt(dx, dy);
      return normalizedPoint(point.x, point.y, transform);
    };
    const auto bottomLeft = corner(-halfWidth, halfHeight);
    const auto bottomRight = corner(halfWidth, halfHeight);
    const auto topLeft = corner(-halfWidth, -halfHeight);
    const auto topRight = corner(halfWidth, -halfHeight);
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
                                   .touchScratch = element.touchScratch,
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
