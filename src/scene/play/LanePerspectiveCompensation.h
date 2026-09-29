#pragma once

#include <cmath>

namespace gameplay_scroll_geometry {

// Perspective-correct interpolation along the lane. A linear scroll position
// denotes a fraction of the projected judgement-to-top segment, rather than
// a fraction of its world-space length. Depths are camera-forward distances;
// their common scale cancels, so the forward vector need not be normalized.
struct LanePerspectiveCompensation {
  float judgeY;
  float topY;
  float judgeDepth;
  float topDepth;

  [[nodiscard]] float toWorld(float linearY) const {
    if (!valid() || !std::isfinite(linearY)) {
      return linearY;
    }
    const double s = (static_cast<double>(linearY) - judgeY) / (topY - judgeY);
    // Continue with endpoint tangents outside the lane. Traversal can hand
    // us distant rows and clipped long-note heads; extrapolating the rational
    // curve there could cross its pole and wrap them back into view.
    const double t = s < 0.0 ? s * judgeDepth / topDepth
                     : s > 1.0 ? 1.0 + (s - 1.0) * topDepth / judgeDepth
                               : s * judgeDepth /
                                     ((1.0 - s) * topDepth + s * judgeDepth);
    return static_cast<float>(judgeY + t * (topY - judgeY));
  }

  [[nodiscard]] float toLinear(float worldY) const {
    if (!valid() || !std::isfinite(worldY)) {
      return worldY;
    }
    const double t = (static_cast<double>(worldY) - judgeY) / (topY - judgeY);
    const double s = t < 0.0 ? t * topDepth / judgeDepth
                     : t > 1.0 ? 1.0 + (t - 1.0) * judgeDepth / topDepth
                               : t * topDepth /
                                     ((1.0 - t) * judgeDepth + t * topDepth);
    return static_cast<float>(judgeY + s * (topY - judgeY));
  }

private:
  [[nodiscard]] bool valid() const {
    return std::isfinite(judgeY) && std::isfinite(topY) &&
           std::isfinite(judgeDepth) && std::isfinite(topDepth) &&
           topY > judgeY && judgeDepth > 0.0001F && topDepth > 0.0001F;
  }
};

} // namespace gameplay_scroll_geometry
