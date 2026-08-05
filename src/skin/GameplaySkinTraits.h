#pragma once

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace skin {

// These values are the gameplay entries of Beatoraja's SkinType enum at
// c2ed5db1a46145ed10790c3872f717e95b59db9d.  Keep the skin type rather than
// deriving it from key mode: the enum also distinguishes 24K single and
// double play while charts expose the latter as key mode 48.
struct GameplaySkinTrait {
  int skinType = -1;
  int keyMode = 0;
  std::string_view label;
  bool operator==(const GameplaySkinTrait &) const = default;
};

inline constexpr std::array<GameplaySkinTrait, 7> kGameplaySkinTraits = {{
    {.skinType = 0, .keyMode = 7, .label = "7K"},
    {.skinType = 1, .keyMode = 5, .label = "5K"},
    {.skinType = 2, .keyMode = 14, .label = "14K"},
    {.skinType = 3, .keyMode = 10, .label = "10K"},
    {.skinType = 4, .keyMode = 9, .label = "9K"},
    {.skinType = 16, .keyMode = 24, .label = "24K"},
    {.skinType = 17, .keyMode = 48, .label = "24K Double"},
}};

[[nodiscard]] inline std::span<const GameplaySkinTrait>
gameplaySkinTraits() noexcept {
  return kGameplaySkinTraits;
}

[[nodiscard]] inline std::optional<GameplaySkinTrait>
gameplaySkinTraitForSkinType(int skinType) noexcept {
  for (const auto &trait : kGameplaySkinTraits) {
    if (trait.skinType == skinType) {
      return trait;
    }
  }
  return std::nullopt;
}

[[nodiscard]] inline std::optional<GameplaySkinTrait>
gameplaySkinTraitForKeyMode(int keyMode) noexcept {
  for (const auto &trait : kGameplaySkinTraits) {
    if (trait.keyMode == keyMode) {
      return trait;
    }
  }
  return std::nullopt;
}

} // namespace skin
