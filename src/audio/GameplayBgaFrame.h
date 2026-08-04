#pragma once

#include "../skin/SkinPresentationTypes.h"
#include "../skin/package/SkinPackageTypes.h"

#include <array>
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

// This intentionally duplicates only the four scalar fields required from
// skin::UiLogicalRect.  UiLogicalRect belongs to the optional Beatoraja skin
// model layer, while gameplay BGA submission must also compile without it.
struct GameplayBgaClipRect {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
};

struct BgaPreflightResult {
  bool ready = false;
  std::optional<skin::SkinDiagnostic> failure;
};

struct BgaDrawTarget {
  GameplayBgaRole role = GameplayBgaRole::Base;
  std::uint16_t viewId = 0;
  // Matches SkinDestinationEvaluator's UI quad order: BL, BR, TR, TL.
  std::array<GameplayBgaPoint, 4> destination{};
  skin::SkinStretchMode stretch = skin::SkinStretchMode::Stretch;
  std::array<float, 4> tint{1.0F, 1.0F, 1.0F, 1.0F};
  skin::SkinBlendMode blend = skin::SkinBlendMode::Normal;
  std::optional<GameplayBgaClipRect> clip;
  std::uint32_t authoredOrdinal = 0;
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

// Resource preparation and renderer submission are deliberately separate so
// gameplay stays independent of the optional Beatoraja renderer stack.
class IGameplayBgaSubmitter {
public:
  virtual ~IGameplayBgaSubmitter() = default;

  [[nodiscard]] virtual PreparedGameplayBgaFrame prepareVisualFrameAt(
      std::uint64_t frameSerial, std::int64_t bgaTimeMicros,
      const GameplayBgaMissState &missState) = 0;
  [[nodiscard]] virtual BgaPreflightResult
  preflight(const PreparedGameplayBgaFrame &frame,
            std::span<const BgaDrawTarget> targets) = 0;
  virtual void submitPrepared(const PreparedGameplayBgaFrame &frame,
                              const BgaDrawTarget &target) noexcept = 0;
  virtual void submitFullscreen(const PreparedGameplayBgaFrame &frame) noexcept =
      0;
};
