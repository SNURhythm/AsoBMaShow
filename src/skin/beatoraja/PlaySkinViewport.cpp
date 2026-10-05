#include "PlaySkinViewport.h"

#include <algorithm>
#include <array>
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
  effective.playAreaZoom =
      std::isfinite(effective.playAreaZoom) && effective.playAreaZoom > 0.0F
          ? std::clamp(effective.playAreaZoom,
                       SkinProfileSettingsPolicy::minPlayAreaZoom,
                       SkinProfileSettingsPolicy::maxPlayAreaZoom)
          : 1.0F;
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

UiLogicalRect intersectUiRects(const UiLogicalRect &left,
                               const UiLogicalRect &right) {
  const double minimumX = std::max(left.x, right.x);
  const double minimumY = std::max(left.y, right.y);
  const double maximumX =
      std::min(left.x + left.width, right.x + right.width);
  const double maximumY =
      std::min(left.y + left.height, right.y + right.height);
  return {.x = minimumX,
          .y = minimumY,
          .width = std::max(0.0, maximumX - minimumX),
          .height = std::max(0.0, maximumY - minimumY)};
}

} // namespace

std::optional<AuthoredRect>
playSkinAuthoredPlayArea(const ValidatedBeatorajaSkinModel &model) {
  const SkinNoteObject *source = nullptr;
  for (const auto &object : model.model.objects) {
    if (std::find(model.disabledOptionalObjects.begin(),
                  model.disabledOptionalObjects.end(), object.id) !=
        model.disabledOptionalObjects.end()) continue;
    if (const auto *note = std::get_if<SkinNoteObject>(&object.payload)) {
      source = note;
    }
  }
  std::optional<AuthoredRect> bounds;
  if (!source) return bounds;
  for (const auto &lane : source->lanes) {
    const auto &rect = lane.laneDestination;
    if (lane.authoredLane < 0 || !finite(rect.x) || !finite(rect.y) ||
        !finite(rect.width) || !finite(rect.height) ||
        rect.width <= 0.0 || rect.height <= 0.0) continue;
    if (!bounds) {
      bounds = rect;
    } else {
      const double right = std::max(bounds->x + bounds->width, rect.x + rect.width);
      const double top = std::max(bounds->y + bounds->height, rect.y + rect.height);
      bounds->x = std::min(bounds->x, rect.x);
      bounds->y = std::min(bounds->y, rect.y);
      bounds->width = right - bounds->x;
      bounds->height = top - bounds->y;
    }
  }
  return bounds;
}

PlaySkinViewport evaluatePlaySkinViewport(AuthoredSize authoredSize,
                                          UiLogicalRect safeUiBounds,
                                          const ViewportSettings &settings,
                                          std::optional<AuthoredRect> playArea) {
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
  result.destinationScaleX = scaleX;
  result.destinationScaleY = scaleY;
  double tx = safeUiBounds.x + (safeUiBounds.width - authoredSize.width * scaleX) / 2.0;
  double ty = safeUiBounds.y + (safeUiBounds.height - authoredSize.height * scaleY) / 2.0 +
              authoredSize.height * scaleY;

  // Frame the authored lanes with one camera transform, shared by all skin
  // rendering and interaction. Preserve base destination scaling for authored
  // operations that explicitly use the skin's original logical canvas.
  const bool focusPlayArea = effective.centerPlayArea && playArea &&
      finite(playArea->x) && finite(playArea->y) &&
      finite(playArea->width) && finite(playArea->height) &&
      playArea->width > 0.0 && playArea->height > 0.0;
  if (focusPlayArea) {
    scaleX = scaleY = std::min(safeUiBounds.width / playArea->width,
                              safeUiBounds.height / playArea->height) *
                      effective.playAreaZoom;
    tx = safeUiBounds.x + safeUiBounds.width / 2.0 -
         (playArea->x + playArea->width / 2.0) * scaleX;
    ty = safeUiBounds.y + safeUiBounds.height / 2.0 +
         (playArea->y + playArea->height / 2.0) * scaleY;
  }

  if (effective.mode == ViewportMode::Custom) {
    const double centerX = safeUiBounds.x + safeUiBounds.width / 2.0;
    const double centerY = safeUiBounds.y + safeUiBounds.height / 2.0;
    scaleX *= effective.scaleX;
    scaleY *= effective.scaleY;
    tx = centerX + effective.scaleX * (tx - centerX) + effective.translateX;
    ty = centerY + effective.scaleY * (ty - centerY) + effective.translateY;
  }

  // Authored Y points upward. Once the lanes exceed the safe height, keep
  // their judgment-line edge visible and let the top crop as zoom increases.
  if (focusPlayArea && playArea->height * scaleY > safeUiBounds.height) {
    ty = std::min(ty, safeUiBounds.y + safeUiBounds.height + playArea->y * scaleY);
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
  const std::array canvasCorners{
      transform(result.authoredToUi, 0.0, 0.0),
      transform(result.authoredToUi, authoredSize.width, 0.0),
      transform(result.authoredToUi, authoredSize.width, authoredSize.height),
      transform(result.authoredToUi, 0.0, authoredSize.height)};
  const auto [minimumX, maximumX] = std::minmax_element(
      canvasCorners.begin(), canvasCorners.end(),
      [](const AuthoredPoint &left, const AuthoredPoint &right) {
        return left.x < right.x;
      });
  const auto [minimumY, maximumY] = std::minmax_element(
      canvasCorners.begin(), canvasCorners.end(),
      [](const AuthoredPoint &left, const AuthoredPoint &right) {
        return left.y < right.y;
      });
  result.projectedUiBounds = intersectUiRects(
      {.x = minimumX->x,
       .y = minimumY->y,
       .width = maximumX->x - minimumX->x,
       .height = maximumY->y - minimumY->y},
      safeUiBounds);
  result.valid = true;
  return result;
}

} // namespace skin
