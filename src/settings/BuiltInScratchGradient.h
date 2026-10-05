#pragma once

#include "BuiltInNotes.h"

namespace built_in_notes {
inline bool hasScratchGradient(Type type) {
  return type == Type::Normal || type == Type::LongHead || type == Type::LongTail ||
         type == Type::HellHead || type == Type::HellTail;
}

struct GradientStop {
  float position;
  std::uint32_t color;
};

inline std::array<GradientStop, 7> scratchGradient(std::uint32_t rgb) {
  // A broad, slightly asymmetric satin sheen keeps the chosen hue visible.
  // The GPU interpolates between stops; these are not flat color bands.
  constexpr std::array<float, 7> positions{
      0.0F, 0.18F, 0.38F, 0.52F, 0.66F, 0.84F, 1.0F};
  constexpr std::array<int, 7> lightness{-18, -6, 18, 32, 26, 2, -14};
  std::array<GradientStop, 7> result{};
  for (std::size_t i = 0; i < result.size(); ++i) {
    std::uint32_t color = 0;
    for (const int shift : {16, 8, 0}) {
      const int channel = (rgb >> shift) & 255;
      const int amount = lightness[i];
      const int shaded = amount >= 0 ? channel + (255 - channel) * amount / 100
                                    : channel * (100 + amount) / 100;
      color |= static_cast<std::uint32_t>(shaded) << shift;
    }
    result[i] = {positions[i], color};
  }
  return result;
}
} // namespace built_in_notes
