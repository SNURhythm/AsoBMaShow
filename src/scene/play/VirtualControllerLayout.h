#pragma once

#include "RealtimeTouchInputRouter.h"
#include "../../input/VirtualControllerConfig.h"
#include "../../targets.h"

#include <optional>
#include <vector>

namespace gameplay {

[[nodiscard]] constexpr bool virtualControllerTouchInputSupported() noexcept {
#if TARGET_OS_IPHONE || TARGET_IPHONE_SIMULATOR
  return true;
#else
  return false;
#endif
}

struct VirtualControllerCanvas {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;

  [[nodiscard]] bool valid() const noexcept;
  bool operator==(const VirtualControllerCanvas &) const = default;
};

struct VirtualControllerRect {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] float centerX() const noexcept { return x + width * 0.5F; }
  [[nodiscard]] float centerY() const noexcept { return y + height * 0.5F; }
};

enum class VirtualControllerControl : unsigned char {
  Scratch,
  Key,
  Start,
  Select,
};

enum class VirtualControllerShape : unsigned char {
  Circle,
  Rectangle,
};

// Rotate a screen-space offset clockwise about the origin.
[[nodiscard]] RealtimeTouchPoint rotateControllerOffset(float x, float y, float degrees) noexcept;

struct VirtualControllerElement {
  VirtualControllerControl control = VirtualControllerControl::Key;
  VirtualControllerShape shape = VirtualControllerShape::Rectangle;
  int keyPosition = -1;
  int lane = -1;
  bool scratch = false;
  bool spinScratch = false;
  bool touchScratch = false;
  // A 1P platter is drawn on the left. An upward flick along its practical
  // right-hand edge is counter-clockwise, unlike the historical generic
  // vertical-swipe mapping used by skin-authored scratch lanes.
  bool invertFlickScratchDirection = false;
  std::optional<replay::LogicalControl> replayControl;
  VirtualControllerRect bounds;
  float rotationDegrees = 0;
  [[nodiscard]] RealtimeTouchPoint pointAt(float dx, float dy) const noexcept {
    const auto offset = rotateControllerOffset(dx, dy, rotationDegrees);
    return {bounds.centerX() + offset.x, bounds.centerY() + offset.y};
  }

  [[nodiscard]] int configIndex() const noexcept {
    switch (control) {
    case VirtualControllerControl::Scratch: return input::VirtualControllerConfig::kScratchControl;
    case VirtualControllerControl::Start: return input::VirtualControllerConfig::kStartControl;
    case VirtualControllerControl::Select: return input::VirtualControllerConfig::kSelectControl;
    case VirtualControllerControl::Key: return keyPosition;
    }
    return -1;
  }
};

struct VirtualControllerLayout {
  VirtualControllerRect bounds;
  std::vector<VirtualControllerElement> elements;
  RealtimeTouchPoint pivot;
  float plateHalfHeight = 0;
  float unit = 0.0F;
  float touchCompensation = 0.5F;

  [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] bool supportsVirtualControllerKeyMode(int keyMode) noexcept;

// The controller owns 4/5/6/7/8-key single-play and either independently selected
// deck on 10/14-key double-play. It uses the chart's canonical lane mapping
// rather than a visual-column-to-lane assumption.
[[nodiscard]] VirtualControllerLayout makeVirtualControllerLayout(
    const input::VirtualControllerConfig &config, int keyMode,
    VirtualControllerCanvas canvas);

// Scene-owned: callers must consume the reference before requesting a layout
// with different inputs. Pressed state/rotation do not affect this geometry.
class VirtualControllerLayoutCache {
public:
  [[nodiscard]] const VirtualControllerLayout &get(
      const input::VirtualControllerConfig &config, int keyMode,
      VirtualControllerCanvas canvas);

private:
  input::VirtualControllerConfig config_;
  int keyMode_ = 0;
  VirtualControllerCanvas canvas_;
  bool initialized_ = false;
  VirtualControllerLayout layout_;
};

[[nodiscard]] std::vector<RealtimeTouchLaneRegion>
makeVirtualControllerTouchRegions(const VirtualControllerLayout &layout,
                                  const RealtimeTouchUiTransform &transform);

} // namespace gameplay
