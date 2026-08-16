#include "SkinCoverNormalization.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "LuaSkinTableDecoder.h"

#include <cmath>

namespace skin {
namespace {

SkinCoverNormalizationResult
failure(SkinCoverNormalizationError error) {
  return {.cover = std::nullopt,
          .destinationOffsetIds = {},
          .error = error};
}

bool validSprite(const SkinSpriteFrames &sprite) {
  if (sprite.resource == 0 || sprite.frames.empty() ||
      sprite.frames.size() >
          LuaSkinTableDecoderPolicy::maxMaterializedSpriteFrames ||
      (sprite.timer && !*sprite.timer)) {
    return false;
  }
  for (const auto &frame : sprite.frames) {
    if (frame.gridColumns <= 0 || frame.gridRows <= 0 || frame.gridColumn < 0 ||
        frame.gridRow < 0 || frame.gridColumn >= frame.gridColumns ||
        frame.gridRow >= frame.gridRows || frame.w < -1 || frame.h < -1) {
      return false;
    }
  }
  return true;
}

bool validLineScale(double value) {
  return std::isfinite(value) && value > 0.0 &&
         value <= SkinCoverNormalizationPolicy::maxScaledDisappearLine;
}

bool validDisappearLine(double value) {
  return std::isfinite(value) &&
         std::abs(value) <= SkinCoverNormalizationPolicy::maxScaledDisappearLine;
}

} // namespace

SkinCoverNormalizationResult
normalizeSkinCover(const SkinCoverNormalizationInput &input) {
  if (!validSprite(input.sprite)) {
    return failure(SkinCoverNormalizationError::InvalidSprite);
  }
  if (input.authoredDestinationOffsetIds.size() >
      SkinCoverNormalizationPolicy::maxAuthoredDestinationOffsetIds) {
    return failure(
        SkinCoverNormalizationError::DestinationOffsetLimitExceeded);
  }

  // JsonPlaySkinObjectLoader always calls setDisapearLine with the field's
  // default -1, multiplied by skin.getScaleY(). Keep that scaling even when a
  // source format omitted the line field; any negative result remains
  // runtime-unclipped in SkinHidden.
  if (!validLineScale(input.lineScale)) {
    return failure(SkinCoverNormalizationError::InvalidLineScale);
  }
  const double authoredDisappearLine =
      input.authoredDisappearLine.value_or(-1.0);
  if (!validDisappearLine(authoredDisappearLine)) {
    return failure(SkinCoverNormalizationError::InvalidDisappearLine);
  }
  const double disappearLine = authoredDisappearLine * input.lineScale;
  if (!validDisappearLine(disappearLine)) {
    return failure(SkinCoverNormalizationError::InvalidDisappearLine);
  }

  SkinCoverObject cover;
  cover.kind = input.kind;
  cover.sprite = input.sprite;
  cover.disappearLine = disappearLine;
  cover.disappearLineLinksLift =
      input.authoredDisappearLineLinksLift.value_or(
          input.kind == SkinCoverKind::Hidden);

  std::vector<int> offsets = input.authoredDestinationOffsetIds;
  offsets.push_back(kSkinCoverLiftOffsetId);
  if (input.kind == SkinCoverKind::Hidden) {
    offsets.push_back(kSkinCoverHiddenOffsetId);
  }
  return {.cover = std::move(cover),
          .destinationOffsetIds = std::move(offsets),
          .error = SkinCoverNormalizationError::None};
}

} // namespace skin

#endif
