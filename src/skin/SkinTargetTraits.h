#pragma once

#include "GameplaySkinTraits.h"

#include <array>
#include <optional>
#include <string_view>

namespace skin {

enum class SkinTargetKind { Gameplay, MusicSelect, Result, CourseResult };

struct SkinTargetTrait {
  int skinType = -1;
  SkinTargetKind kind = SkinTargetKind::Gameplay;
  int keyMode = 0;
  std::string_view label;
};

inline constexpr std::array<SkinTargetTrait, 10> kSkinTargetTraits = {{
    {0, SkinTargetKind::Gameplay, 7, "7K1S"},
    {1, SkinTargetKind::Gameplay, 5, "5K1S"},
    {2, SkinTargetKind::Gameplay, 14, "7K DP"},
    {3, SkinTargetKind::Gameplay, 10, "5K DP"},
    {4, SkinTargetKind::Gameplay, 9, "9K"},
    {5, SkinTargetKind::MusicSelect, 0, "Music Select"},
    {7, SkinTargetKind::Result, 0, "Result"},
    {15, SkinTargetKind::CourseResult, 0, "Course Result"},
    {16, SkinTargetKind::Gameplay, 24, "24K"},
    {17, SkinTargetKind::Gameplay, 48, "24K Double"},
}};

// Application settings identities are independent of the compatible source type.
[[nodiscard]] constexpr bool isAdditionalGameplaySkinTarget(int type) noexcept {
  return type == -4 || type == -5 || type == -6 || type == -7 || type == -8;
}

[[nodiscard]] constexpr int skinSourceTypeForTarget(int type) noexcept {
  return (type == -4 || type == -5) ? 1 : (type == -6 || type == -7 || type == -8) ? 0 : type;
}

[[nodiscard]] constexpr const auto &skinTargetTraits() noexcept {
  return kSkinTargetTraits;
}

[[nodiscard]] constexpr std::optional<SkinTargetTrait>
skinTargetTraitForType(int skinType) noexcept {
  switch (skinType) {
  case -5: return SkinTargetTrait{-5, SkinTargetKind::Gameplay, 5, "5K"};
  case -7: return SkinTargetTrait{-7, SkinTargetKind::Gameplay, 7, "7K"};
  case -4: return SkinTargetTrait{-4, SkinTargetKind::Gameplay, 4, "4K"};
  case -6: return SkinTargetTrait{-6, SkinTargetKind::Gameplay, 6, "6K"};
  case -8: return SkinTargetTrait{-8, SkinTargetKind::Gameplay, 8, "8K"};
  default: break;
  }
  for (const auto &trait : kSkinTargetTraits) {
    if (trait.skinType == skinType) return trait;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr std::optional<SkinTargetTrait>
gameplaySkinTargetForKeyMode(int keyMode) noexcept {
  if (keyMode == -5 || keyMode == -7) return skinTargetTraitForType(keyMode);
  if (keyMode == 4 || keyMode == 6 || keyMode == 8) {
    return skinTargetTraitForType(-keyMode);
  }
  for (const auto &trait : kSkinTargetTraits) {
    if (trait.kind == SkinTargetKind::Gameplay && trait.keyMode == keyMode) {
      return trait;
    }
  }
  return std::nullopt;
}

} // namespace skin
