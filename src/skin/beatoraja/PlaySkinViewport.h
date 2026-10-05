#pragma once

#include "../SkinProfileSettings.h"
#include "BeatorajaSkinModel.h"

#include <optional>

namespace skin {

struct AuthoredSize {
  double width = 0.0;
  double height = 0.0;
};

struct AuthoredPoint {
  double x = 0.0;
  double y = 0.0;
};

using AuthoredRect = SkinAuthoredRect;

struct UiLogicalRect {
  double x = 0.0;
  double y = 0.0;
  double width = 0.0;
  double height = 0.0;
};

struct Affine2D {
  double m00 = 1.0;
  double m01 = 0.0;
  double tx = 0.0;
  double m10 = 0.0;
  double m11 = 1.0;
  double ty = 0.0;
};

struct PlaySkinViewport {
  Affine2D authoredToUi;
  Affine2D uiToAuthored;
  AuthoredRect drawableAuthoredBounds;
  UiLogicalRect safeUiBounds;
  std::optional<UiLogicalRect> projectedUiBounds;
  bool valid = false;
  // Selected logical destination canvas, before the optional custom transform.
  double destinationScaleX = 1.0;
  double destinationScaleY = 1.0;
};

// Transient compensation for a vertically cropped lane. Authored geometry,
// configured Hi-Speed and the user's cover percentage remain unchanged.
struct PlaySkinVisibleScroll {
  double authoredLaneHeight = 0.0;
  double originY = 0.0;
  double authoredHeight = 0.0;
  double visibleBottomY = 0.0;
  double visibleTopY = 0.0;
  double height = 0.0;
  double scale = 1.0;
  double topCrop = 0.0;
};

std::optional<PlaySkinVisibleScroll>
playSkinVisibleScroll(const ValidatedBeatorajaSkinModel &, const PlaySkinViewport &,
                      double liftRatio = 0.0);

// Same last enabled Note source used by the renderer for lane interaction.
std::optional<AuthoredRect>
playSkinAuthoredPlayArea(const ValidatedBeatorajaSkinModel &model);

PlaySkinViewport evaluatePlaySkinViewport(AuthoredSize authoredSize,
                                          UiLogicalRect safeUiBounds,
                                          const ViewportSettings &settings,
                                          std::optional<AuthoredRect> playArea = std::nullopt);

inline const UiLogicalRect &
projectedSkinScissorBounds(const PlaySkinViewport &viewport) noexcept {
  return viewport.projectedUiBounds ? *viewport.projectedUiBounds
                                    : viewport.safeUiBounds;
}

} // namespace skin
