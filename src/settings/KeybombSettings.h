#pragma once

#include "../scene/play/JudgementColors.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace keybomb {
struct Style {
  bool visible = true;
  int sizePercent = 100;
  int opacityPercent = 100;
  std::optional<std::uint32_t> color; // RGB override; absent keeps the judgement palette.
  bool operator==(const Style &) const = default;
};
using Settings = std::array<Style, 6>;
inline Style sanitize(Style style) {
  style.sizePercent = std::clamp(style.sizePercent, 25, 300);
  style.opacityPercent = std::clamp(style.opacityPercent, 0, 100);
  if (style.color) *style.color &= 0xFFFFFFU;
  return style;
}
inline bool valid(Judgement judge) { return judge >= PGreat && judge <= Poor; }
inline std::uint32_t defaultColor(Judgement judge, bool light = false) {
  return judgement_colors::rgb(judge, light);
}
inline std::uint32_t resolvedColor(Judgement judge, const Style &style, bool light = false) {
  return style.color.value_or(defaultColor(judge, light));
}
inline std::uint32_t effectColor(Judgement judge, const Style &style) {
  const auto rgb = resolvedColor(judge, style);
  return ((rgb & 0xFFU) << 16) | (rgb & 0xFF00U) | ((rgb >> 16) & 0xFFU);
}
} // namespace keybomb
