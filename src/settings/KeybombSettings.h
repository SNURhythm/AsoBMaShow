#pragma once

#include "../scene/play/Judgement.h"
#include <algorithm>
#include <array>

namespace keybomb {
struct Style {
  bool visible = true;
  int sizePercent = 100;
  int opacityPercent = 100;
  bool operator==(const Style &) const = default;
};
using Settings = std::array<Style, 6>;
inline Style sanitize(Style style) {
  style.sizePercent = std::clamp(style.sizePercent, 25, 300);
  style.opacityPercent = std::clamp(style.opacityPercent, 0, 100);
  return style;
}
inline bool valid(Judgement judge) { return judge >= PGreat && judge <= Poor; }
} // namespace keybomb
