#pragma once

#include "../skin/SkinPresentationTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

inline constexpr int kGameplayBgaAuthoredBlank = -1;
inline constexpr std::int64_t kDefaultMissLayerDurationMicros = 500'000;

struct GameplayBgaMissState {
  bool active = false;
  std::int64_t startedBgaMicros = 0;
  std::int64_t durationMicros = kDefaultMissLayerDurationMicros;
  std::uint64_t triggerSerial = 0;

  [[nodiscard]] bool isActiveAt(std::int64_t bgaTimeMicros) const noexcept;
  [[nodiscard]] std::optional<std::size_t>
  frameIndexAt(std::int64_t bgaTimeMicros,
               std::size_t frameCount) const noexcept;
};

enum class GameplayBgaRole : std::uint8_t { Base, Layer, Miss };

enum class GameplayBgaComposition : std::uint8_t {
  Blank,
  MissOnly,
  BaseThenLayer,
};

// This is intentionally render-neutral: resource lookup and materialization
// belong to the presentation adapter, after compatibility selection.
struct GameplayBgaMissCompositionSelection {
  GameplayBgaComposition composition = GameplayBgaComposition::BaseThenLayer;
  std::optional<int> resourceId;
};

// Mirrors BGAProcessor.drawBGA at Beatoraja c2ed5db1: while an authored poor
// sequence is active, it suppresses base/layer even if the selected frame is
// blank or has no materialized resource. The zero start sentinel and exclusive
// end come from GameplayBgaMissState::isActiveAt/frameIndexAt.
[[nodiscard]] inline GameplayBgaMissCompositionSelection
SelectGameplayBgaMissComposition(
    std::optional<std::span<const int>> authoredPoorFrames,
    const GameplayBgaMissState &missState,
    std::int64_t bgaTimeMicros) noexcept {
  if (!authoredPoorFrames.has_value() || !missState.isActiveAt(bgaTimeMicros)) {
    return {};
  }

  GameplayBgaMissCompositionSelection selection{
      .composition = GameplayBgaComposition::MissOnly};
  const auto frameIndex =
      missState.frameIndexAt(bgaTimeMicros, authoredPoorFrames->size());
  if (!frameIndex.has_value()) {
    return selection;
  }

  const int resourceId = (*authoredPoorFrames)[*frameIndex];
  if (resourceId != kGameplayBgaAuthoredBlank) {
    selection.resourceId = resourceId;
  }
  return selection;
}

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
