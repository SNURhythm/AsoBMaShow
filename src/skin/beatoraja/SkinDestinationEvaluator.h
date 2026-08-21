#pragma once

#include "PlaySkinViewport.h"
#include "../SkinProfileSettings.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace skin {

// MainController owns SkinOffset values as floats. Persisted skin-profile
// offsets remain integral ConfigOffset values, but LaneRenderer writes the
// reserved gameplay offsets (Lift, lane cover, and HIDDEN) from scaled lane
// geometry every frame, so their render-time representation must retain the
// source precision.
struct SkinRuntimeOffset {
  double x = 0.0;
  double y = 0.0;
  double w = 0.0;
  double h = 0.0;
  double r = 0.0;
  double a = 0.0;
};

[[nodiscard]] constexpr SkinRuntimeOffset
skinRuntimeOffset(const ConfigOffset &offset) noexcept {
  return {.x = static_cast<double>(offset.x),
          .y = static_cast<double>(offset.y),
          .w = static_cast<double>(offset.w),
          .h = static_cast<double>(offset.h),
          .r = static_cast<double>(offset.r),
          .a = static_cast<double>(offset.a)};
}

struct SkinLaneCoverStateView {
  bool supported = false;
  bool laneCoverEnabled = false;
  double laneCover = 0.0;
  bool liftEnabled = false;
  double lift = 0.0;
  bool hiddenEnabled = false;
  double hidden = 0.0;
};

struct SkinDestinationEvaluationInputs {
  std::int64_t nowMicros = 0;
  std::int64_t timerStartMicros = INT64_MIN;
  // TimerProperty.isOff() and TimerProperty.get() are independent calls in
  // pinned Beatoraja. A raw INT64_MIN value remains a valid read when the
  // preceding OFF probe returned false.
  bool timerOff = false;
  // One resolved value per SkinDestinationBody::conditions entry in authored
  // order, followed by drawCondition when that condition is present.
  std::span<const bool> optionConditions;
  // Already-resolved values in SkinDestinationBody::offsetIds authored order.
  std::span<const SkinRuntimeOffset> orderedOffsets;
};

struct AuthoredDestinationGeometry {
  AuthoredRect rect;
  std::optional<AuthoredRect> clip;
  double centerX = 0.0;
  double centerY = 0.0;
  double angleDegrees = 0.0;
  // Beatoraja interpolates normalized float color components.  Quantization
  // is deliberately deferred to the renderer upload boundary.
  std::array<float, 4> rgba{1.0F, 1.0F, 1.0F, 1.0F};
  SkinBlendMode blend = SkinBlendMode::Normal;
  SkinFilterMode filter = SkinFilterMode::Nearest;
  SkinStretchMode stretch = SkinStretchMode::Stretch;
};

struct SkinSourceRegionGeometry {
  int textureWidth = 0;
  int textureHeight = 0;
  SkinSourceRect region;
};

struct SkinDestinationEvaluationResult {
  std::optional<AuthoredDestinationGeometry> geometry;
  std::vector<SkinDiagnostic> diagnostics;
};

struct UiDestinationGeometry {
  // Vertices and UVs are BL, BR, TR, TL in corresponding order.
  std::array<std::array<double, 2>, 4> vertices{};
  std::array<std::array<double, 2>, 4> normalizedUvs{};
  std::optional<UiLogicalRect> clip;
  std::array<float, 4> rgba{1.0F, 1.0F, 1.0F, 1.0F};
  SkinBlendMode blend = SkinBlendMode::Normal;
  SkinFilterMode filter = SkinFilterMode::Nearest;
};

struct SkinStretchedDestinationGeometry {
  AuthoredRect rect;
  SkinSourceRect region;
};

SkinDestinationEvaluationResult
evaluateSkinDestinationAuthored(const SkinDestinationBody &destination,
                                const SkinDestinationEvaluationInputs &inputs);

[[nodiscard]] SkinStretchedDestinationGeometry
stretchSkinDestinationAuthored(const AuthoredDestinationGeometry &destination,
                               const SkinSourceRegionGeometry &source);

UiDestinationGeometry
projectSkinDestinationToUi(const AuthoredDestinationGeometry &destination,
                           const SkinSourceRegionGeometry &source,
                           const PlaySkinViewport &viewport);

} // namespace skin
