#pragma once

#include "BuiltInNotes.h"

namespace built_in_lane {
struct Style {
  std::uint32_t measureLineColor = 0xFFFFFF;
  int measureLineThicknessPercent = 100;
  int backgroundOpacityPercent = 48;
  bool operator==(const Style &) const = default;
};
using Settings = std::map<int, Style>;

inline Style sanitizeStyle(Style style) {
  style.measureLineColor &= 0xFFFFFFU;
  style.measureLineThicknessPercent = std::clamp(style.measureLineThicknessPercent, 25, 500);
  style.backgroundOpacityPercent = std::clamp(style.backgroundOpacityPercent, 0, 100);
  return style;
}
inline float measureLineHeight(Style style) {
  return 0.05F * sanitizeStyle(style).measureLineThicknessPercent / 100.0F;
}
inline std::uint8_t backgroundAlpha(Style style) {
  return static_cast<std::uint8_t>((sanitizeStyle(style).backgroundOpacityPercent * 255 + 50) / 100);
}
inline void sanitize(Settings &settings) {
  std::erase_if(settings, [](const auto &entry) {
    return std::find(built_in_notes::kModes.begin(), built_in_notes::kModes.end(), entry.first) ==
           built_in_notes::kModes.end();
  });
  for (auto &[mode, style] : settings) style = sanitizeStyle(style);
}
} // namespace built_in_lane
