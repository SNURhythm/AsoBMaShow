#pragma once

#include "../skin/SkinPresentationTypes.h"

#include <cstdint>
#include <optional>

inline constexpr int kGameplayBgaAuthoredBlank = -1;

enum class GameplayBgaRole : std::uint8_t { Base, Layer, Miss };

enum class GameplayBgaComposition : std::uint8_t {
  Blank,
  MissOnly,
  BaseThenLayer,
};

struct GameplayBgaPoint {
  float x = 0.0F;
  float y = 0.0F;
};

enum class GameplayBgaMediaKind : std::uint8_t { Image, Video };

struct PreparedGameplayBgaSurface {
  GameplayBgaRole role = GameplayBgaRole::Base;
  GameplayBgaMediaKind mediaKind = GameplayBgaMediaKind::Image;
  std::uint64_t surfaceToken = 0;
  int sourceWidth = 0;
  int sourceHeight = 0;
};

struct PreparedGameplayBgaFrame {
  std::uint64_t sequence = 0;
  GameplayBgaComposition composition = GameplayBgaComposition::Blank;
  std::optional<PreparedGameplayBgaSurface> base;
  std::optional<PreparedGameplayBgaSurface> layer;
  std::optional<PreparedGameplayBgaSurface> miss;
};
