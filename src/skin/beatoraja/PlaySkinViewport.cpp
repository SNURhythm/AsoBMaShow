#include "PlaySkinViewport.h"

#include <algorithm>
#include <cmath>

namespace skin {
namespace {

bool finite(double value) { return std::isfinite(value); }

ViewportSettings effectiveSettings(const ViewportSettings &settings) {
  const bool validMode = settings.mode == ViewportMode::Fit ||
                         settings.mode == ViewportMode::Stretch ||
                         settings.mode == ViewportMode::Custom;
  const bool validBase = settings.customBase == CustomViewportBase::Fit ||
                         settings.customBase == CustomViewportBase::Stretch;
  const bool validNumbers = finite(settings.scaleX) && finite(settings.scaleY) &&
                            finite(settings.translateX) && finite(settings.translateY) &&
                            settings.scaleX > 0.0F && settings.scaleY > 0.0F;
  if (!validMode || !validBase || !validNumbers) {
    return {};
  }
  auto effective = settings;
  effective.scaleX = std::clamp(effective.scaleX,
                                SkinProfileSettingsPolicy::minCustomScale,
                                SkinProfileSettingsPolicy::maxCustomScale);
  effective.scaleY = std::clamp(effective.scaleY,
                                SkinProfileSettingsPolicy::minCustomScale,
                                SkinProfileSettingsPolicy::maxCustomScale);
  effective.translateX = std::clamp(
      effective.translateX, SkinProfileSettingsPolicy::minCustomTranslation,
      SkinProfileSettingsPolicy::maxCustomTranslation);
  effective.translateY = std::clamp(
      effective.translateY, SkinProfileSettingsPolicy::minCustomTranslation,
      SkinProfileSettingsPolicy::maxCustomTranslation);
  return effective;
}

bool invert(const Affine2D &affine, Affine2D &inverse) {
  const double determinant = affine.m00 * affine.m11 - affine.m01 * affine.m10;
  if (!finite(determinant) || determinant == 0.0) {
    return false;
  }
  inverse.m00 = affine.m11 / determinant;
  inverse.m01 = -affine.m01 / determinant;
  inverse.m10 = -affine.m10 / determinant;
  inverse.m11 = affine.m00 / determinant;
  inverse.tx = -(inverse.m00 * affine.tx + inverse.m01 * affine.ty);
  inverse.ty = -(inverse.m10 * affine.tx + inverse.m11 * affine.ty);
  return finite(inverse.m00) && finite(inverse.m01) && finite(inverse.m10) &&
         finite(inverse.m11) && finite(inverse.tx) && finite(inverse.ty);
}

AuthoredPoint transform(const Affine2D &affine, double x, double y) {
  return {.x = affine.m00 * x + affine.m01 * y + affine.tx,
          .y = affine.m10 * x + affine.m11 * y + affine.ty};
}

} // namespace

PlaySkinViewport evaluatePlaySkinViewport(AuthoredSize authoredSize,
                                          UiLogicalRect safeUiBounds,
                                          const ViewportSettings &settings) {
  PlaySkinViewport result;
  result.safeUiBounds = safeUiBounds;
  if (!finite(authoredSize.width) || !finite(authoredSize.height) ||
      !finite(safeUiBounds.x) || !finite(safeUiBounds.y) ||
      !finite(safeUiBounds.width) || !finite(safeUiBounds.height) ||
      authoredSize.width <= 0.0 || authoredSize.height <= 0.0 ||
      safeUiBounds.width <= 0.0 || safeUiBounds.height <= 0.0) {
    return result;
  }

  const auto effective = effectiveSettings(settings);
  const bool stretch = effective.mode == ViewportMode::Stretch ||
                       (effective.mode == ViewportMode::Custom &&
                        effective.customBase == CustomViewportBase::Stretch);
  double scaleX = safeUiBounds.width / authoredSize.width;
  double scaleY = safeUiBounds.height / authoredSize.height;
  if (!stretch) {
    scaleX = scaleY = std::min(scaleX, scaleY);
  }
  double tx = safeUiBounds.x + (safeUiBounds.width - authoredSize.width * scaleX) / 2.0;
  double ty = safeUiBounds.y + (safeUiBounds.height - authoredSize.height * scaleY) / 2.0 +
              authoredSize.height * scaleY;

  if (effective.mode == ViewportMode::Custom) {
    const double centerX = safeUiBounds.x + safeUiBounds.width / 2.0;
    const double centerY = safeUiBounds.y + safeUiBounds.height / 2.0;
    scaleX *= effective.scaleX;
    scaleY *= effective.scaleY;
    tx = centerX + effective.scaleX * (tx - centerX) + effective.translateX;
    ty = centerY + effective.scaleY * (ty - centerY) + effective.translateY;
  }

  result.authoredToUi = {.m00 = scaleX, .m01 = 0.0, .tx = tx,
                         .m10 = 0.0, .m11 = -scaleY, .ty = ty};
  if (!invert(result.authoredToUi, result.uiToAuthored)) {
    return result;
  }
  const auto topLeft = transform(result.uiToAuthored, safeUiBounds.x, safeUiBounds.y);
  const auto bottomRight = transform(result.uiToAuthored,
                                     safeUiBounds.x + safeUiBounds.width,
                                     safeUiBounds.y + safeUiBounds.height);
  result.drawableAuthoredBounds = {
      .x = std::min(topLeft.x, bottomRight.x),
      .y = std::min(topLeft.y, bottomRight.y),
      .width = std::abs(bottomRight.x - topLeft.x),
      .height = std::abs(bottomRight.y - topLeft.y),
  };
  result.valid = true;
  return result;
}

} // namespace skin
