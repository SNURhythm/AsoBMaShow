#pragma once

#include <compare>
#include <string>
#include <vector>

namespace input {

// Values are fractions of the current mobile canvas. The layout code derives
// pixels from the shorter canvas edge so a profile feels consistent across
// landscape iPhones and iPads.
struct VirtualControllerConfig {
  static constexpr float kDefaultCenterX = 0.50F;
  static constexpr float kDefaultCenterY = 0.76F;
  static constexpr float kDefaultButtonSize = 0.095F;
  // Each spacing value is an edge-to-edge distance in the indicated button
  // dimension. They are intentionally signed: negative values overlap
  // controls on that axis, matching common two-row arcade layouts.
  static constexpr float kDefaultKeySpacingX = -0.35F;
  static constexpr float kDefaultKeySpacingY = 0.20F;
  static constexpr float kDefaultScratchKeyplateSpacing = 0.25F;

  bool enabled = false;
  float centerX = kDefaultCenterX;
  float centerY = kDefaultCenterY;
  float buttonSize = kDefaultButtonSize;
  float keySpacingX = kDefaultKeySpacingX;
  float keySpacingY = kDefaultKeySpacingY;
  float scratchKeyplateSpacing = kDefaultScratchKeyplateSpacing;

  auto operator<=>(const VirtualControllerConfig &) const = default;

  void sanitize(std::vector<std::string> &diagnostics);
};

} // namespace input
