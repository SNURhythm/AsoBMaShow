#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace built_in_notes {

enum class Type {
  Normal, LongHead, LongTail, LongBodyOff, LongBodyOn,
  HellHead, HellTail, HellBodyOff, HellBodyOn, HellDamage, Mine, Invisible
};
enum class Palette { Gray, Blue, Scratch };
inline constexpr std::array<int, 12> kModes{4, -5, 5, 6, -7, 7, 8, 9, 10, 14, 24, 48};
inline constexpr int kMaxLanes = 48;
inline constexpr std::array<const char *, 12> kTypeNames{
    "normal", "long_head", "long_tail", "long_body_off", "long_body_on",
    "hcn_head", "hcn_tail", "hcn_body_off", "hcn_body_on", "hcn_damage",
    "mine", "invisible"};
inline constexpr int kMinThickness = 25;
inline constexpr int kMaxThickness = 300;
struct Style {
  std::uint32_t color = 0xCCCCCC;
  int thickness = 100;
  bool operator==(const Style &) const = default;
};
using LaneStyles = std::map<Type, Style>;
using ModeStyles = std::map<int, LaneStyles>;
using Settings = std::map<int, ModeStyles>;

inline bool isBody(Type type) {
  return type == Type::LongBodyOff || type == Type::LongBodyOn ||
         type == Type::HellBodyOff || type == Type::HellBodyOn ||
         type == Type::HellDamage;
}
inline Style defaultStyle(Palette palette, Type type) {
  // Raw RGB samples from simple_gray.png, simple_blue.png and orange.png.
  // Scratch gradients use representative colored pixels, excluding their
  // white highlights and black borders; decoration is drawn separately.
  constexpr std::array<std::array<std::uint32_t, 12>, 3> colors{{
      {0xCCCCCC, 0xCCCCCC, 0xCCCCCC, 0x999999, 0xCCCCCC,
       0xD9D9D9, 0xD9D9D9, 0xA3A3A3, 0xD9D9D9, 0xCC0000, 0xCC0000, 0xFF9524},
      {0x3399CC, 0x3399CC, 0x3399CC, 0x267399, 0x3399CC,
       0x33BFCC, 0x33BFCC, 0x268F99, 0x33BFCC, 0xCC0000, 0xCC0000, 0xFF9524},
      {0xDB3625, 0xDB3625, 0x6D5F5E, 0x8B5449, 0xDC7864,
       0xDB2551, 0x6D5E62, 0x8B4954, 0xDC6478, 0xDC3F2F, 0xBD1E1D, 0xFF9524},
  }};
  return {colors[static_cast<int>(palette)][static_cast<int>(type)], 100};
}
inline Style resolve(const ModeStyles &settings, int lane, Type type,
                     Palette palette) {
  if (const auto found = settings.find(lane); found != settings.end()) {
    if (const auto style = found->second.find(type); style != found->second.end())
      return {style->second.color & 0xFFFFFFU,
              std::clamp(style->second.thickness, kMinThickness, kMaxThickness)};
  }
  return defaultStyle(palette, type);
}
inline float height(float laneWidth, Type type, const Style &style) {
  // Normal notes and mines have 20 blank rows in a 128 x 40 sprite.
  const float pixels = type == Type::Normal || type == Type::Mine ? 20.0F : 40.0F;
  return laneWidth * pixels / 128.0F *
         std::clamp(style.thickness, kMinThickness, kMaxThickness) / 100.0F;
}
inline float bodyWidth(float laneWidth, const Style &style) {
  return laneWidth * std::clamp(style.thickness, kMinThickness, 100) / 100.0F;
}
inline std::uint32_t abgr(std::uint32_t rgb) {
  return 0xFF000000U | ((rgb & 0xFFU) << 16) | (rgb & 0xFF00U) | ((rgb >> 16) & 0xFFU);
}
inline std::string colorHex(std::uint32_t rgb) {
  char buffer[7];
  std::snprintf(buffer, sizeof(buffer), "%06X", rgb & 0xFFFFFFU);
  return buffer;
}
inline std::optional<std::uint32_t> parseColor(std::string_view text) {
  if (text.starts_with('#')) text.remove_prefix(1);
  if (text.size() != 6) return std::nullopt;
  std::uint32_t result = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result, 16);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
  return result;
}
inline void sanitize(Settings &settings) {
  std::erase_if(settings, [](auto &entry) {
    return std::find(kModes.begin(), kModes.end(), entry.first) == kModes.end();
  });
  for (auto &[mode, lanes] : settings) {
    std::erase_if(lanes, [](auto &entry) { return entry.first < 0 || entry.first >= kMaxLanes; });
    for (auto &[lane, types] : lanes) {
      std::erase_if(types, [](auto &entry) {
        return static_cast<unsigned>(entry.first) >= kTypeNames.size();
      });
      for (auto &[type, style] : types) {
        style.color &= 0xFFFFFFU;
        style.thickness = std::clamp(style.thickness, kMinThickness,
                                     isBody(type) ? 100 : kMaxThickness);
      }
    }
  }
}
} // namespace built_in_notes
