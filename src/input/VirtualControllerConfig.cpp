#include "VirtualControllerConfig.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace input {
namespace {

constexpr float kMinimumButtonSize = 0.025F;
constexpr float kMaximumButtonSize = 0.30F;
// Keep the key pitch positive while still allowing nearly complete overlap.
// The ranges are deliberately broad; they only prevent an unusable inverted
// layout or an unbounded profile value from making the editor non-interactive.
constexpr float kMinimumKeySpacing = -0.95F;
constexpr float kMaximumKeySpacing = 4.0F;
constexpr float kMinimumScratchKeyplateSpacing = -3.0F;
constexpr float kMaximumScratchKeyplateSpacing = 4.0F;

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
  keySpacingX = finiteOrDefault(keySpacingX, kDefaultKeySpacingX,
                                "horizontal key spacing", diagnostics);
  keySpacingY = finiteOrDefault(keySpacingY, kDefaultKeySpacingY,
                                "vertical key spacing", diagnostics);
  scratchKeyplateSpacing = finiteOrDefault(
      scratchKeyplateSpacing, kDefaultScratchKeyplateSpacing,
      "scratch-to-keyplate spacing", diagnostics);

  clampValue(centerX, 0.0F, 1.0F, "horizontal position", diagnostics);
  clampValue(centerY, 0.0F, 1.0F, "vertical position", diagnostics);
  clampValue(buttonSize, kMinimumButtonSize, kMaximumButtonSize, "button size",
             diagnostics);
  clampValue(keySpacingX, kMinimumKeySpacing, kMaximumKeySpacing,
             "horizontal key spacing", diagnostics);
  clampValue(keySpacingY, kMinimumKeySpacing, kMaximumKeySpacing,
             "vertical key spacing", diagnostics);
  clampValue(scratchKeyplateSpacing, kMinimumScratchKeyplateSpacing,
             kMaximumScratchKeyplateSpacing, "scratch-to-keyplate spacing",
             diagnostics);
}

} // namespace input
