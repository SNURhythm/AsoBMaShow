#pragma once

#include <compare>
#include <cstdint>

namespace skin {

struct SkinFloatWriterId {
  std::uint32_t value = 0;

  explicit operator bool() const noexcept { return value != 0; }
  auto operator<=>(const SkinFloatWriterId &) const = default;
};

enum class SkinBlendMode : std::uint8_t {
  Normal,
  Additive,
  Subtractive,
  Multiply,
  Inverse,
};

enum class SkinFilterMode : std::uint8_t {
  Nearest,
  Linear,
};

enum class SkinStretchMode : std::uint8_t {
  Stretch = 0,
  KeepAspectRatioFitInner = 1,
  KeepAspectRatioFitOuter = 2,
  KeepAspectRatioFitOuterTrimmed = 3,
  KeepAspectRatioFitWidth = 4,
  KeepAspectRatioFitWidthTrimmed = 5,
  KeepAspectRatioFitHeight = 6,
  KeepAspectRatioFitHeightTrimmed = 7,
  KeepAspectRatioNoExpanding = 8,
  NoResize = 9,
  NoResizeTrimmed = 10,
};

} // namespace skin
