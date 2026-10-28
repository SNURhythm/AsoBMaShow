#pragma once

#include <array>
#include <cmath>
#include <numbers>

namespace rendering {
struct UnitCircleSample {
  float x;
  float y;
};

template <int Segments>
const std::array<UnitCircleSample, Segments + 1> &unitCircleSamples() {
  static_assert(Segments > 0);
  static const auto samples = [] {
    std::array<UnitCircleSample, Segments + 1> result{};
    for (int i = 0; i <= Segments; ++i) {
      // Preserve the original float expression and separately evaluated seam.
      const float angle = 2 * std::numbers::pi_v<float> * i / Segments;
      result[i] = {std::cos(angle), std::sin(angle)};
    }
    return result;
  }();
  return samples;
}
} // namespace rendering
