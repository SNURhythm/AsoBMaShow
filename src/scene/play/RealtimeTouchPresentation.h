#pragma once

#include "RealtimeTouchInputRouter.h"
#include "../../rendering/common.h"

namespace gameplay {

[[nodiscard]] inline RealtimeTouchPoint
realtimeTouchPresentationPoint(float screenNormalizedX,
                               float screenNormalizedY) noexcept {
  RealtimeTouchPoint point;
  rendering::normalizedToUiNormalized(screenNormalizedX, screenNormalizedY,
                                      point.x, point.y);
  return point;
}

} // namespace gameplay
