#pragma once

#include "RealtimeTouchInputRouter.h"
#include "../../rendering/common.h"

namespace gameplay {

[[nodiscard]] inline bool realtimeTouchAllowsLegacyBuiltInControl(
    const PresentationUiHit &hit) noexcept {
  return hit.kind == PresentationUiControlKind::None ||
         hit.permitsLegacyBuiltInFallback;
}

[[nodiscard]] inline UiLogicalPoint
realtimeTouchUiLogicalPoint(float screenNormalizedX,
                            float screenNormalizedY) noexcept {
  UiLogicalPoint point;
  rendering::normalizedToUi(screenNormalizedX, screenNormalizedY, point.x,
                            point.y);
  return point;
}

[[nodiscard]] inline RealtimeTouchPoint
realtimeTouchPresentationPoint(float screenNormalizedX,
                               float screenNormalizedY) noexcept {
  RealtimeTouchPoint point;
  rendering::normalizedToUiNormalized(screenNormalizedX, screenNormalizedY,
                                      point.x, point.y);
  return point;
}

} // namespace gameplay
