#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace color_picker {
struct Hsv {
  float hue = 0;
  float saturation = 0;
  float value = 1;
  bool operator==(const Hsv &) const = default;
};

inline std::uint32_t toRgb(Hsv hsv) {
  const float hue = std::clamp(hsv.hue, 0.0F, 1.0F) * 6.0F;
  const float saturation = std::clamp(hsv.saturation, 0.0F, 1.0F);
  const float value = std::clamp(hsv.value, 0.0F, 1.0F);
  const int sector = static_cast<int>(hue) % 6;
  const float fraction = hue - std::floor(hue);
  const float p = value * (1 - saturation);
  const float q = value * (1 - saturation * fraction);
  const float t = value * (1 - saturation * (1 - fraction));
  const std::array<std::array<float, 3>, 6> colors{{
      {value, t, p}, {q, value, p}, {p, value, t},
      {p, q, value}, {t, p, value}, {value, p, q}}};
  const auto &rgb = colors[sector];
  return (static_cast<std::uint32_t>(std::lround(rgb[0] * 255)) << 16U) |
         (static_cast<std::uint32_t>(std::lround(rgb[1] * 255)) << 8U) |
         static_cast<std::uint32_t>(std::lround(rgb[2] * 255));
}

inline Hsv fromRgb(std::uint32_t rgb, Hsv previous = {}) {
  const float r = float((rgb >> 16U) & 255U) / 255.0F;
  const float g = float((rgb >> 8U) & 255U) / 255.0F;
  const float b = float(rgb & 255U) / 255.0F;
  const float maximum = std::max({r, g, b});
  const float minimum = std::min({r, g, b});
  const float difference = maximum - minimum;
  Hsv hsv{.hue = previous.hue,
          .saturation = maximum == 0 ? previous.saturation : difference / maximum,
          .value = maximum};
  if (difference == 0) return hsv;
  float hue = maximum == r ? (g - b) / difference
            : maximum == g ? (b - r) / difference + 2.0F
                           : (r - g) / difference + 4.0F;
  if (hue < 0) hue += 6.0F;
  hsv.hue = hue / 6.0F;
  return hsv;
}
} // namespace color_picker
