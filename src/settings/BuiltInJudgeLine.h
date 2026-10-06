#pragma once

#include "BuiltInNotes.h"

namespace built_in_judge_line {
struct Style {
  std::uint32_t color = 0xFFFFFF;
  int heightPercent = 100;
  bool operator==(const Style &) const = default;
};
using Settings = std::map<int, Style>;
inline constexpr int kMinHeight = 25;
inline constexpr int kMaxHeight = 500;

inline Style sanitizeStyle(Style style) {
  style.color &= 0xFFFFFFU;
  style.heightPercent = std::clamp(style.heightPercent, kMinHeight, kMaxHeight);
  return style;
}
inline float height(float laneWidth, Style style) {
  return laneWidth * 20.0F / 128.0F * sanitizeStyle(style).heightPercent / 100.0F;
}
inline void sanitize(Settings &settings) {
  std::erase_if(settings, [](const auto &entry) {
    return std::find(built_in_notes::kModes.begin(), built_in_notes::kModes.end(), entry.first) ==
           built_in_notes::kModes.end();
  });
  for (auto &[mode, style] : settings) style = sanitizeStyle(style);
}
} // namespace built_in_judge_line
