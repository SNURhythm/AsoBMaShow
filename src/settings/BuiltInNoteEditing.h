#pragma once

#include "BuiltInNotes.h"
#include <span>
#include <vector>

namespace built_in_notes {
inline std::vector<std::uint32_t> colorPresets(std::uint32_t defaultColor) {
  constexpr std::array<std::uint32_t, 11> colors{
      0xCCCCCC, 0xFFFFFF, 0x3399CC, 0x33BFCC, 0xDB3625, 0xCC0000,
      0xFF9524, 0xFFDD55, 0x66DD88, 0xAA88FF, 0xFF77BB};
  std::vector<std::uint32_t> result{defaultColor};
  for (const auto color : colors)
    if (std::find(result.begin(), result.end(), color) == result.end()) result.push_back(color);
  return result;
}

struct LaneTarget {
  int lane;
  Palette palette;
};
enum class EditKind { Color, Thickness, AdjustThickness, ResetColor, ResetThickness };
struct CommonStyle {
  std::optional<std::uint32_t> color;
  std::optional<int> thickness;
};

inline CommonStyle commonStyle(const ModeStyles &settings,
                               std::span<const LaneTarget> targets, Type type) {
  if (targets.empty()) return {};
  const auto first = resolve(settings, targets.front().lane, type, targets.front().palette);
  CommonStyle result{first.color, first.thickness};
  for (const auto &target : targets.subspan(1)) {
    const auto style = resolve(settings, target.lane, type, target.palette);
    if (result.color != style.color) result.color.reset();
    if (result.thickness != style.thickness) result.thickness.reset();
  }
  return result;
}

inline void editSelected(ModeStyles &settings, std::span<const LaneTarget> targets,
                         Type type, EditKind kind, int value = 0) {
  for (const auto &target : targets) {
    auto style = resolve(settings, target.lane, type, target.palette);
    const int maximum = isBody(type) ? 100 : kMaxThickness;
    switch (kind) {
    case EditKind::Color: style.color = static_cast<std::uint32_t>(value) & 0xFFFFFFU; break;
    case EditKind::Thickness: style.thickness = std::clamp(value, kMinThickness, maximum); break;
    case EditKind::AdjustThickness:
      style.thickness = static_cast<int>(std::clamp<std::int64_t>(
          std::int64_t(style.thickness) + value, kMinThickness, maximum));
      break;
    case EditKind::ResetColor: style.color = defaultStyle(target.palette, type).color; break;
    case EditKind::ResetThickness: style.thickness = 100; break;
    }
    settings[target.lane][type] = style;
  }
}
} // namespace built_in_notes
