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

inline std::array<GradientStop, 10> scratchGradient(std::uint32_t rgb) {
  // orange.png's horizontal sheen peaks around x=40/128, with darker edges.
  // Mix with white/black so custom colors retain the same highlight profile.
  constexpr std::array<float, 10> positions{
      0.0F, 0.125F, 0.1875F, 0.25F, 0.3125F, 0.375F, 0.5F, 0.625F, 0.75F, 1.0F};
  constexpr std::array<int, 10> lightness{-20, -5, 15, 50, 90, 30, 0, -18, -32, -20};
  std::array<GradientStop, 10> result{};
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
