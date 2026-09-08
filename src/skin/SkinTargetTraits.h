#pragma once

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
    {0, SkinTargetKind::Gameplay, 7, "7K"},
    {1, SkinTargetKind::Gameplay, 5, "5K"},
    {2, SkinTargetKind::Gameplay, 14, "14K"},
    {3, SkinTargetKind::Gameplay, 10, "10K"},
    {4, SkinTargetKind::Gameplay, 9, "9K"},
    {5, SkinTargetKind::MusicSelect, 0, "Music Select"},
    {7, SkinTargetKind::Result, 0, "Result"},
    {15, SkinTargetKind::CourseResult, 0, "Course Result"},
    {16, SkinTargetKind::Gameplay, 24, "24K"},
    {17, SkinTargetKind::Gameplay, 48, "24K Double"},
}};

[[nodiscard]] constexpr const auto &skinTargetTraits() noexcept {
  return kSkinTargetTraits;
}

[[nodiscard]] constexpr std::optional<SkinTargetTrait>
skinTargetTraitForType(int skinType) noexcept {
  for (const auto &trait : kSkinTargetTraits) {
    if (trait.skinType == skinType) return trait;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr std::optional<SkinTargetTrait>
gameplaySkinTargetForKeyMode(int keyMode) noexcept {
  for (const auto &trait : kSkinTargetTraits) {
    if (trait.kind == SkinTargetKind::Gameplay && trait.keyMode == keyMode) {
      return trait;
    }
  }
  return std::nullopt;
}

} // namespace skin
