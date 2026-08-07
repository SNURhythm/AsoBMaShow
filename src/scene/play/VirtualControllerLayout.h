#pragma once

#include "RealtimeTouchInputRouter.h"
#include "../../input/VirtualControllerConfig.h"

#include <optional>
#include <vector>

namespace gameplay {

struct VirtualControllerCanvas {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;

  [[nodiscard]] bool valid() const noexcept;
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

struct VirtualControllerElement {
  VirtualControllerControl control = VirtualControllerControl::Key;
  VirtualControllerShape shape = VirtualControllerShape::Rectangle;
  int keyPosition = -1;
  int lane = -1;
  bool scratch = false;
  std::optional<replay::LogicalControl> replayControl;
  VirtualControllerRect bounds;
};

struct VirtualControllerLayout {
  VirtualControllerRect bounds;
  std::vector<VirtualControllerElement> elements;

  [[nodiscard]] bool valid() const noexcept;
};

[[nodiscard]] bool supportsVirtualControllerKeyMode(int keyMode) noexcept;

// The controller intentionally owns only 5-key and 7-key single-play. The
// layout mirrors their physical button rows and uses the chart's canonical
// lane mapping rather than a visual-column-to-lane assumption.
[[nodiscard]] VirtualControllerLayout makeVirtualControllerLayout(
    const input::VirtualControllerConfig &config, int keyMode,
    VirtualControllerCanvas canvas);

[[nodiscard]] std::vector<RealtimeTouchLaneRegion>
makeVirtualControllerTouchRegions(const VirtualControllerLayout &layout,
                                  const RealtimeTouchUiTransform &transform);

} // namespace gameplay
