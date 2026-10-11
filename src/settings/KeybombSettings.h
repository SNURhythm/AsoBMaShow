#pragma once

#include "../scene/play/Judgement.h"
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
inline std::uint32_t defaultColor(Judgement judge) {
  switch (judge) {
  case PGreat: return 0xA0E8FF;
  case Great: return 0xFFDC6E;
  case Good: return 0x9CE3B2;
  case Bad: return 0xF09278;
  case Poor: return 0xC26C62;
  default: return 0xA187BE;
  }
}
inline std::uint32_t resolvedColor(Judgement judge, const Style &style) {
  return style.color.value_or(defaultColor(judge));
}
inline std::uint32_t effectColor(Judgement judge, const Style &style) {
  const auto rgb = resolvedColor(judge, style);
  return ((rgb & 0xFFU) << 16) | (rgb & 0xFF00U) | ((rgb >> 16) & 0xFFU);
}
} // namespace keybomb
