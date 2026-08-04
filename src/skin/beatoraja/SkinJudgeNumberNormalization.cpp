#include "SkinJudgeNumberNormalization.h"

#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include <cmath>
#include <limits>
#include <utility>

namespace skin {
namespace {

SkinJudgeNumberNormalizationResult
failure(SkinJudgeNumberNormalizationError error) {
  return {.number = std::nullopt, .error = error};
}

bool finiteOffset(const SkinDigitOffset &offset) noexcept {
  return std::isfinite(offset.x) && std::isfinite(offset.y) &&
         std::isfinite(offset.width) && std::isfinite(offset.height);
}

bool finiteDestination(const SkinDestinationBody &destination) noexcept {
  if (destination.mouseRect &&
      (!std::isfinite(destination.mouseRect->x) ||
       !std::isfinite(destination.mouseRect->y) ||
       !std::isfinite(destination.mouseRect->width) ||
       !std::isfinite(destination.mouseRect->height))) {
    return false;
  }
  for (const auto &frame : destination.frames) {
    if (!std::isfinite(frame.x) || !std::isfinite(frame.y) ||
        !std::isfinite(frame.width) || !std::isfinite(frame.height) ||
        !std::isfinite(frame.angleDegrees)) {
      return false;
    }
  }
  return true;
}

bool isIntGeometry(double value) noexcept {
  return std::isfinite(value) && std::trunc(value) == value &&
         value >= static_cast<double>(std::numeric_limits<int>::min()) &&
         value <= static_cast<double>(std::numeric_limits<int>::max());
}

} // namespace

SkinJudgeNumberNormalizationResult
normalizeSkinJudgeNumber(const SkinJudgeNumberNormalizationInput &input) {
  const std::size_t sourceFrames = input.source.frames.size();
  if (sourceFrames == 0) {
    return failure(SkinJudgeNumberNormalizationError::EmptyFrames);
  }
  if (sourceFrames > SkinJudgeNumberNormalizationPolicy::maxMaterializedFrames) {
    return failure(SkinJudgeNumberNormalizationError::FrameLimitExceeded);
  }
  if (input.offsets.size() > SkinJudgeNumberNormalizationPolicy::maxDigitOffsets) {
    return failure(SkinJudgeNumberNormalizationError::OffsetLimitExceeded);
  }
  if (!input.value) {
    return failure(SkinJudgeNumberNormalizationError::MissingValueBinding);
  }
  if (input.digitCount < 0 ||
      input.digitCount > SkinJudgeNumberNormalizationPolicy::maxDigitCount) {
    return failure(SkinJudgeNumberNormalizationError::InvalidDigitCount);
  }
  if (!finiteDestination(input.destination)) {
    return failure(SkinJudgeNumberNormalizationError::NonFiniteGeometry);
  }
  for (const auto &offset : input.offsets) {
    if (!finiteOffset(offset)) {
      return failure(SkinJudgeNumberNormalizationError::NonFiniteGeometry);
    }
  }

  // Pinned Java chooses d solely from divisibility by ten. In particular, it
  // does not use generic Number's 24-glyph signed-layout precedence.
  const int glyphsPerRow = sourceFrames % 10U == 0U ? 10 : 11;
  const std::size_t completeFrames =
      sourceFrames / static_cast<std::size_t>(glyphsPerRow) *
      static_cast<std::size_t>(glyphsPerRow);
  if (completeFrames == 0) {
    return failure(SkinJudgeNumberNormalizationError::EmptyFrames);
  }

  SkinDestinationBody destination = input.destination;
  for (auto &frame : destination.frames) {
    if (!isIntGeometry(frame.x) || !isIntGeometry(frame.width)) {
      return failure(
          SkinJudgeNumberNormalizationError::InvalidIntegerGeometry);
    }
    const auto shiftedX =
        static_cast<std::int64_t>(static_cast<int>(frame.x)) -
        static_cast<std::int64_t>(static_cast<int>(frame.width)) *
            static_cast<std::int64_t>(input.digitCount) / 2;
    if (shiftedX < std::numeric_limits<int>::min() ||
        shiftedX > std::numeric_limits<int>::max()) {
      return failure(
          SkinJudgeNumberNormalizationError::InvalidIntegerGeometry);
    }
    frame.x = static_cast<double>(shiftedX);
  }

  SkinSpriteFrames digits = input.source;
  digits.frames.resize(completeFrames);
  SkinNumberObject number;
  number.digits.positive = std::move(digits);
  number.digits.glyphsPerAnimationFrame = glyphsPerRow;
  number.value = input.value;
  number.digitCount = input.digitCount;
  number.spacing = input.spacing;
  number.alignment = 2;
  number.relativeToJudgeImage = true;
  number.zeroPadding = glyphsPerRow == 11
                           ? SkinZeroPaddingMode::AlternateZero
                           : SkinZeroPaddingMode::None;
  number.perDigitOffsets = input.offsets;

  return {.number = SkinJudgeNumberPresentation{.number = std::move(number),
                                                 .destination = std::move(destination)},
          .error = SkinJudgeNumberNormalizationError::None};
}

} // namespace skin

#endif
