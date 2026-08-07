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
  static constexpr float kDefaultKeyGap = 0.20F;

  bool enabled = false;
  float centerX = kDefaultCenterX;
  float centerY = kDefaultCenterY;
  float buttonSize = kDefaultButtonSize;
  float keyGap = kDefaultKeyGap;

  auto operator<=>(const VirtualControllerConfig &) const = default;

  void sanitize(std::vector<std::string> &diagnostics);
};

} // namespace input
