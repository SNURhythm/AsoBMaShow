#include "VirtualControllerConfig.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace input {
namespace {

constexpr float kMinimumButtonSize = 0.025F;
constexpr float kMaximumButtonSize = 0.30F;
constexpr float kMinimumKeyGap = 0.0F;
constexpr float kMaximumKeyGap = 1.50F;

float finiteOrDefault(float value, float fallback, const char *label,
                      std::vector<std::string> &diagnostics) {
  if (std::isfinite(value)) {
    return value;
  }
  diagnostics.emplace_back("Reset non-finite virtual controller " +
                           std::string(label) + ".");
  return fallback;
}

void clampValue(float &value, float minimum, float maximum, const char *label,
                std::vector<std::string> &diagnostics) {
  const float clamped = std::clamp(value, minimum, maximum);
  if (clamped != value) {
    value = clamped;
    diagnostics.emplace_back("Clamped virtual controller " +
                             std::string(label) + ".");
  }
}

} // namespace

void VirtualControllerConfig::sanitize(std::vector<std::string> &diagnostics) {
  centerX = finiteOrDefault(centerX, kDefaultCenterX, "horizontal position",
                            diagnostics);
  centerY = finiteOrDefault(centerY, kDefaultCenterY, "vertical position",
                            diagnostics);
  buttonSize =
      finiteOrDefault(buttonSize, kDefaultButtonSize, "button size", diagnostics);
  keyGap = finiteOrDefault(keyGap, kDefaultKeyGap, "key spacing", diagnostics);

  clampValue(centerX, 0.0F, 1.0F, "horizontal position", diagnostics);
  clampValue(centerY, 0.0F, 1.0F, "vertical position", diagnostics);
  clampValue(buttonSize, kMinimumButtonSize, kMaximumButtonSize, "button size",
             diagnostics);
  clampValue(keyGap, kMinimumKeyGap, kMaximumKeyGap, "key spacing",
             diagnostics);
}

} // namespace input
