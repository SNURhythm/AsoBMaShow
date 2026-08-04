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

// Render-neutral authored geometry retained until the selected BGA surface
// supplies its intrinsic dimensions. Stretch is therefore resolved before
// this authored-to-UI affine, including nonuniform scale and shear.
struct GameplayBgaAffine2D {
  double m00 = 1.0;
  double m01 = 0.0;
  double tx = 0.0;
  double m10 = 0.0;
  double m11 = 1.0;
  double ty = 0.0;
};

struct GameplayBgaAuthoredProjection {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
  double centerX = 0.5;
  double centerY = 0.5;
  double angleDegrees = 0.0;
  GameplayBgaAffine2D authoredToUi;
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

// Byte-oriented because embedded BGA and skin quads use different vertex
// layouts. Padding is the conservative aggregate needed to align every BGA
// allocation before the skin renderer makes its one capacity decision.
struct GameplayBgaTransientRequirements {
  std::uint64_t vertexBytes = 0;
  std::uint64_t vertexAlignmentPadding = 0;
  std::uint64_t indexCount = 0;

  [[nodiscard]] bool empty() const noexcept {
    return vertexBytes == 0 && vertexAlignmentPadding == 0 && indexCount == 0;
  }
};

struct BgaPreflightResult {
  bool ready = false;
  std::optional<skin::SkinDiagnostic> failure;
  GameplayBgaTransientRequirements requirements;
};

struct BgaDrawTarget {
  GameplayBgaRole role = GameplayBgaRole::Base;
  std::uint16_t viewId = 0;
  // Matches SkinDestinationEvaluator's UI quad order: BL, BR, TR, TL.
  std::array<GameplayBgaPoint, 4> destination{};
  skin::SkinStretchMode stretch = skin::SkinStretchMode::Stretch;
  std::optional<GameplayBgaAuthoredProjection> authoredProjection;
  std::array<float, 4> tint{1.0F, 1.0F, 1.0F, 1.0F};
  skin::SkinBlendMode blend = skin::SkinBlendMode::Normal;
  std::optional<GameplayBgaClipRect> clip;
  std::uint32_t authoredOrdinal = 0;
};

enum class GameplayBgaMediaKind : std::uint8_t { Image, Video };

// Mirrors Beatoraja's renderer selection for BGAProcessor.drawBGA. Role wins
// for miss because poor/miss media is always drawn through TYPE_LINEAR,
// including when the selected resource is a movie.
enum class GameplayBgaReferenceRendererType : std::uint8_t {
  Linear,
  Layer,
  Ffmpeg,
};

struct GameplayBgaMaterial {
  // Beatoraja's selected SkinObjectRenderer type. This is reference metadata,
  // not a claim that planar YUV can bypass this backend's conversion shader.
  GameplayBgaReferenceRendererType referenceRendererType =
      GameplayBgaReferenceRendererType::Linear;
  bool linearSampling = true;
  bool blackTransparent = false;
};

[[nodiscard]] constexpr GameplayBgaMaterial
SelectGameplayBgaMaterial(GameplayBgaRole role,
                          GameplayBgaMediaKind mediaKind) noexcept {
  if (role == GameplayBgaRole::Miss) {
    return {};
  }
  if (mediaKind == GameplayBgaMediaKind::Video) {
    return {.referenceRendererType = GameplayBgaReferenceRendererType::Ffmpeg,
            .linearSampling = true,
            .blackTransparent = false};
  }
  if (role == GameplayBgaRole::Layer) {
    return {.referenceRendererType = GameplayBgaReferenceRendererType::Layer,
            .linearSampling = false,
            .blackTransparent = true};
  }
  return {};
}

[[nodiscard]] constexpr std::array<float, 4>
ApplyGameplayBgaBrightness(const std::array<float, 4> &authoredTint,
                           float multiplier) noexcept {
  return {authoredTint[0] * multiplier, authoredTint[1] * multiplier,
          authoredTint[2] * multiplier, authoredTint[3]};
}

// Audit-friendly scalar equivalent of fs_skin_yuvrgb.sc: YUV conversion is
// performed first, then one uniform authored RGBA tint multiplies the result.
[[nodiscard]] constexpr std::array<float, 4>
EvaluateGameplayBgaYuvTint(float y, float u, float v,
                           const std::array<float, 4> &tint) noexcept {
  const float centeredU = u - 0.5F;
  const float centeredV = v - 0.5F;
  return {(y + 1.402F * centeredV) * tint[0],
          (y - 0.344F * centeredU - 0.714F * centeredV) * tint[1],
          (y + 1.772F * centeredU) * tint[2], tint[3]};
}

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

enum class GameplayBgaCompositeMode : std::uint8_t {
  FullscreenBuiltIn,
  EmbeddedSkin,
};

// Reset before each scene render. Gameplay replaces this value only with the
// result for that exact frame; main reuses `prepared` for fullscreen fallback
// without advancing video a second time.
struct GameplayBgaCompositeState {
  std::uint64_t frameSerial = 0;
  GameplayBgaCompositeMode mode =
      GameplayBgaCompositeMode::FullscreenBuiltIn;
  std::optional<PreparedGameplayBgaFrame> prepared;
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
  // commitPrepared is called only after the compositor's combined capacity
  // decision succeeds. It materializes the already validated plan without
  // lookup, decoding, shader creation, or any other fallible work.
  virtual void
  commitPrepared(const PreparedGameplayBgaFrame &frame) noexcept = 0;
  virtual void submitPrepared(const PreparedGameplayBgaFrame &frame,
                              const BgaDrawTarget &target) noexcept = 0;
  // Successful embedded composition and same-frame fullscreen fallback both
  // release the media lease explicitly.
  virtual void
  finalizePrepared(const PreparedGameplayBgaFrame &frame) noexcept = 0;
  virtual void submitFullscreen(const PreparedGameplayBgaFrame &frame) noexcept =
      0;
};
