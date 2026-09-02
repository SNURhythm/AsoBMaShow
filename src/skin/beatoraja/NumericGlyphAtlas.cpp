#include "../LuaGameplaySkinFeature.h"

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS

#include "NumericGlyphAtlas.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace skin {
namespace {

constexpr std::size_t positiveOnlyOutputGlyphs(int inputGlyphs) {
  return static_cast<std::size_t>(inputGlyphs);
}

bool exceedsProductLimit(std::size_t left, std::size_t right,
                         std::size_t limit) noexcept {
  return left != 0 && (right > limit / left);
}

SkinZeroPaddingMode normalizePadding(int value) noexcept {
  return value >= 2 ? SkinZeroPaddingMode::AlternateZero
                    : value >= 1 ? SkinZeroPaddingMode::Zero
                                 : SkinZeroPaddingMode::None;
}

int nonNegative(int value) noexcept { return std::max(value, 0); }

NumericGlyphAtlasResult failure(NumericGlyphAtlasError error) {
  return {.error = error};
}

bool validKind(NumericGlyphAtlasKind kind) noexcept {
  return kind == NumericGlyphAtlasKind::Number ||
         kind == NumericGlyphAtlasKind::Float;
}

bool finiteOffsets(const std::vector<SkinDigitOffset> &offsets) noexcept {
  return std::ranges::all_of(offsets, [](const auto &offset) {
    return std::isfinite(offset.x) && std::isfinite(offset.y) &&
           std::isfinite(offset.width) && std::isfinite(offset.height);
  });
}

bool finiteFormat(const NumericGlyphFormatRequest &format) noexcept {
  return std::isfinite(format.gain) &&
         finiteOffsets(format.perDigitOffsets);
}

bool validPadding(SkinZeroPaddingMode padding) noexcept {
  return padding == SkinZeroPaddingMode::None ||
         padding == SkinZeroPaddingMode::Zero ||
         padding == SkinZeroPaddingMode::AlternateZero;
}

std::size_t maxOffsetsFor(NumericGlyphAtlasKind kind) noexcept {
  return kind == NumericGlyphAtlasKind::Float
             ? NumericGlyphAtlasPolicy::maxFloatDigitOffsets
             : NumericGlyphAtlasPolicy::maxNumberDigitOffsets;
}

SkinSpriteFrames makeSprite(const SkinSpriteFrames &source,
                            std::vector<SkinSourceRect> frames) {
  SkinSpriteFrames result = source;
  result.frames = std::move(frames);
  return result;
}

void appendGlyphs(std::vector<SkinSourceRect> &output,
                  const std::vector<SkinSourceRect> &source,
                  std::size_t rowOffset, std::initializer_list<int> offsets) {
  for (const int offset : offsets) {
    output.push_back(source[rowOffset + static_cast<std::size_t>(offset)]);
  }
}

NumericGlyphAtlasError validateSet(const SkinSpriteFrames &set,
                                   int glyphsPerFrame,
                                   NumericGlyphAtlasBudget budget,
                                   std::size_t *animationFrames) noexcept {
  const auto count = set.frames.size();
  if (count == 0) {
    return NumericGlyphAtlasError::EmptyFrames;
  }
  if (count > budget.remainingMaterializedFrames) {
    return NumericGlyphAtlasError::OutputLimitExceeded;
  }
  if (glyphsPerFrame <= 0) {
    return NumericGlyphAtlasError::ArithmeticOverflow;
  }
  const auto glyphs = static_cast<std::size_t>(glyphsPerFrame);
  if (glyphs > budget.remainingMaterializedFrames) {
    return NumericGlyphAtlasError::ArithmeticOverflow;
  }
  if (count % glyphs != 0) {
    return NumericGlyphAtlasError::UnsupportedGlyphLayout;
  }
  const auto frames = count / glyphs;
  if (exceedsProductLimit(frames, glyphs,
                          budget.remainingMaterializedFrames)) {
    return NumericGlyphAtlasError::OutputLimitExceeded;
  }
  *animationFrames = frames;
  return NumericGlyphAtlasError::None;
}

NumericGlyphAtlasError validateGlyphSetShape(const SkinDigitSpriteSet &atlas,
                                             NumericGlyphAtlasKind *kind) {
  if (kind != nullptr && !validKind(*kind)) {
    return NumericGlyphAtlasError::InvalidKind;
  }
  const int glyphs = atlas.glyphsPerAnimationFrame;
  if (glyphs <= 0 ||
      static_cast<std::size_t>(glyphs) >
          NumericGlyphAtlasPolicy::maxMaterializedFrames) {
    return NumericGlyphAtlasError::ArithmeticOverflow;
  }
  if (kind == nullptr) {
    return glyphs >= 10 && glyphs <= 13
               ? NumericGlyphAtlasError::None
               : NumericGlyphAtlasError::InvalidGlyphSet;
  }
  if (*kind == NumericGlyphAtlasKind::Number) {
    if (glyphs < 10 || glyphs > 12 ||
        (glyphs == 12) != atlas.negative.has_value()) {
      return NumericGlyphAtlasError::InvalidGlyphSet;
    }
  } else if ((glyphs != 12 && glyphs != 13) ||
             (glyphs == 13 && !atlas.negative)) {
    return NumericGlyphAtlasError::InvalidGlyphSet;
  }
  return NumericGlyphAtlasError::None;
}

NumericGlyphAtlasError validateNumericGlyphAtlasImpl(
    const SkinDigitSpriteSet &atlas, NumericGlyphAtlasKind *kind,
    NumericGlyphAtlasBudget budget) noexcept {
  const auto shape = validateGlyphSetShape(atlas, kind);
  if (shape != NumericGlyphAtlasError::None) {
    return shape;
  }
  std::size_t positiveFrames = 0;
  const auto positive = validateSet(atlas.positive,
                                    atlas.glyphsPerAnimationFrame, budget,
                                    &positiveFrames);
  if (positive != NumericGlyphAtlasError::None) {
    return positive;
  }
  if (!atlas.negative) {
    return NumericGlyphAtlasError::None;
  }
  std::size_t negativeFrames = 0;
  const auto negative = validateSet(*atlas.negative,
                                    atlas.glyphsPerAnimationFrame, budget,
                                    &negativeFrames);
  if (negative != NumericGlyphAtlasError::None) {
    return negative;
  }
  if (atlas.negative->frames.size() >
      budget.remainingMaterializedFrames - atlas.positive.frames.size()) {
    return NumericGlyphAtlasError::OutputLimitExceeded;
  }
  return positiveFrames == negativeFrames
             ? NumericGlyphAtlasError::None
             : NumericGlyphAtlasError::UnequalAnimationFrames;
}

bool combinedOutputExceedsLimit(std::size_t animationFrames,
                                std::size_t glyphsAcrossAllSets,
                                NumericGlyphAtlasBudget budget) noexcept {
  return exceedsProductLimit(animationFrames, glyphsAcrossAllSets,
                             budget.remainingMaterializedFrames);
}

NumericGlyphFormat normalizeFormat(const NumericGlyphFormatRequest &input,
                                   NumericGlyphAtlasKind kind,
                                   SkinZeroPaddingMode padding,
                                   bool supportsSign,
                                   bool pinnedLoaderCompatibility) {
  NumericGlyphFormat result;
  result.zeroPadding = padding;
  result.gain = input.gain;
  result.perDigitOffsets = input.perDigitOffsets;
  result.signVisible = supportsSign && input.signVisible;

  int integerDigits =
      pinnedLoaderCompatibility && kind == NumericGlyphAtlasKind::Number
          ? input.integerDigits
          : nonNegative(input.integerDigits);
  int fractionalDigits = nonNegative(input.fractionalDigits);
  if (kind == NumericGlyphAtlasKind::Float) {
    // FloatFormatter gives fractional digits priority when its eight-digit
    // limit is exceeded, without adding the authored values first.
    if (integerDigits >= NumericGlyphAtlasPolicy::maxFloatDigits ||
        fractionalDigits >= NumericGlyphAtlasPolicy::maxFloatDigits ||
        integerDigits > NumericGlyphAtlasPolicy::maxFloatDigits -
                            std::min(fractionalDigits,
                                     NumericGlyphAtlasPolicy::maxFloatDigits)) {
      fractionalDigits =
          std::min(fractionalDigits, NumericGlyphAtlasPolicy::maxFloatDigits);
      integerDigits = NumericGlyphAtlasPolicy::maxFloatDigits -
                      fractionalDigits;
    }
  } else {
    if (!pinnedLoaderCompatibility) {
      integerDigits =
          std::min(integerDigits, NumericGlyphAtlasPolicy::maxNumberDigits);
    }
    fractionalDigits = 0;
  }
  result.integerDigits = integerDigits;
  result.fractionalDigits = fractionalDigits;
  return result;
}

NumericGlyphAtlasResult partitionNumber(const NumericGlyphAtlasRequest &request,
                                        SkinZeroPaddingMode zeroPadding) {
  const auto count = request.source.frames.size();
  const int glyphs = count % 24 == 0 ? 24 : count % 10 == 0 ? 10 : 11;
  if (!request.pinnedLoaderCompatibility &&
      count % static_cast<std::size_t>(glyphs) != 0) {
    return failure(NumericGlyphAtlasError::UnsupportedGlyphLayout);
  }

  NumericGlyphAtlas atlas;
  atlas.format = normalizeFormat(request.format, request.kind,
                                 glyphs == 11 ? SkinZeroPaddingMode::AlternateZero
                                 : glyphs == 10
                                     ? normalizePadding(request.format.numberPadding)
                                     : zeroPadding,
                                 false, request.pinnedLoaderCompatibility);
  if (!request.pinnedLoaderCompatibility &&
      atlas.format.perDigitOffsets.size() > maxOffsetsFor(request.kind)) {
    return failure(NumericGlyphAtlasError::OutputLimitExceeded);
  }
  if (glyphs == 24) {
    const auto animationFrames = count / 24;
    if (!request.pinnedLoaderCompatibility &&
        combinedOutputExceedsLimit(animationFrames, 24, request.budget)) {
      return failure(NumericGlyphAtlasError::OutputLimitExceeded);
    }
    std::vector<SkinSourceRect> positive;
    std::vector<SkinSourceRect> negative;
    positive.reserve(animationFrames * 12);
    negative.reserve(animationFrames * 12);
    for (std::size_t row = 0; row < animationFrames; ++row) {
      const auto offset = row * 24;
      appendGlyphs(positive, request.source.frames, offset,
                   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
      appendGlyphs(negative, request.source.frames, offset,
                   {12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23});
    }
    atlas.digits.positive = makeSprite(request.source, std::move(positive));
    atlas.digits.negative = makeSprite(request.source, std::move(negative));
    atlas.digits.glyphsPerAnimationFrame = 12;
  } else {
    const auto animationFrames = count / static_cast<std::size_t>(glyphs);
    if (!request.pinnedLoaderCompatibility &&
        combinedOutputExceedsLimit(animationFrames,
                                   positiveOnlyOutputGlyphs(glyphs),
                                   request.budget)) {
      return failure(NumericGlyphAtlasError::OutputLimitExceeded);
    }
    atlas.digits.positive = request.source;
    atlas.digits.positive.frames.resize(
        animationFrames * static_cast<std::size_t>(glyphs));
    atlas.digits.glyphsPerAnimationFrame = glyphs;
  }
  if (request.pinnedLoaderCompatibility) {
    return {.atlas = std::move(atlas)};
  }
  const auto validation =
      validateNumericGlyphAtlas(atlas.digits, request.kind, request.budget);
  if (validation != NumericGlyphAtlasError::None) {
    return failure(validation);
  }
  const auto formatValidation = validateNumericGlyphFormat(
      atlas.format, request.kind, atlas.digits.glyphsPerAnimationFrame);
  return formatValidation == NumericGlyphAtlasError::None
             ? NumericGlyphAtlasResult{.atlas = std::move(atlas)}
             : failure(formatValidation);
}

NumericGlyphAtlasResult partitionFloat(const NumericGlyphAtlasRequest &request,
                                       SkinZeroPaddingMode padding) {
  const auto count = request.source.frames.size();
  const int glyphs = count % 26 == 0 ? 26
                     : count % 24 == 0 ? 24
                     : count % 22 == 0 ? 22
                     : count % 12 == 0 ? 12
                     : count % 11 == 0 ? 11
                                      : request.pinnedLoaderCompatibility ? 12
                                                                         : 0;
  if (glyphs == 0) {
    return failure(NumericGlyphAtlasError::UnsupportedGlyphLayout);
  }
  const auto animationFrames = count / static_cast<std::size_t>(glyphs);
  const int outputGlyphs = glyphs == 26 ? 13 : 12;
  const auto outputSets = glyphs == 26 || glyphs == 24 || glyphs == 22 ? 2U
                                                                        : 1U;
  if (!request.pinnedLoaderCompatibility && combinedOutputExceedsLimit(
          animationFrames,
          static_cast<std::size_t>(outputGlyphs) * outputSets,
          request.budget)) {
    return failure(NumericGlyphAtlasError::OutputLimitExceeded);
  }

  NumericGlyphAtlas atlas;
  atlas.format = normalizeFormat(request.format, request.kind, padding,
                                 glyphs == 26,
                                 request.pinnedLoaderCompatibility);
  if (!request.pinnedLoaderCompatibility &&
      atlas.format.perDigitOffsets.size() > maxOffsetsFor(request.kind)) {
    return failure(NumericGlyphAtlasError::OutputLimitExceeded);
  }
  std::vector<SkinSourceRect> positive;
  std::vector<SkinSourceRect> negative;
  positive.reserve(animationFrames * static_cast<std::size_t>(outputGlyphs));
  if (glyphs == 26 || glyphs == 24 || glyphs == 22) {
    negative.reserve(animationFrames * static_cast<std::size_t>(outputGlyphs));
  }
  for (std::size_t row = 0; row < animationFrames; ++row) {
    const auto offset = row * static_cast<std::size_t>(glyphs);
    switch (glyphs) {
    case 26:
      appendGlyphs(positive, request.source.frames, offset,
                   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
      appendGlyphs(negative, request.source.frames, offset,
                   {13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25});
      break;
    case 24:
      appendGlyphs(positive, request.source.frames, offset,
                   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
      appendGlyphs(negative, request.source.frames, offset,
                   {12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23});
      break;
    case 22:
      appendGlyphs(positive, request.source.frames, offset,
                   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 10});
      appendGlyphs(negative, request.source.frames, offset,
                   {11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 11, 21});
      break;
    case 12:
      appendGlyphs(positive, request.source.frames, offset,
                   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11});
      break;
    case 11:
      appendGlyphs(positive, request.source.frames, offset,
                   {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 10});
      break;
    default:
      return failure(NumericGlyphAtlasError::ArithmeticOverflow);
    }
  }
  atlas.digits.positive = makeSprite(request.source, std::move(positive));
  if (!negative.empty()) {
    atlas.digits.negative = makeSprite(request.source, std::move(negative));
  }
  atlas.digits.glyphsPerAnimationFrame = outputGlyphs;
  if (request.pinnedLoaderCompatibility) {
    return {.atlas = std::move(atlas)};
  }
  const auto validation =
      validateNumericGlyphAtlas(atlas.digits, request.kind, request.budget);
  if (validation != NumericGlyphAtlasError::None) {
    return failure(validation);
  }
  const auto formatValidation = validateNumericGlyphFormat(
      atlas.format, request.kind, atlas.digits.glyphsPerAnimationFrame);
  return formatValidation == NumericGlyphAtlasError::None
             ? NumericGlyphAtlasResult{.atlas = std::move(atlas)}
             : failure(formatValidation);
}

} // namespace

NumericGlyphAtlasError validateNumericGlyphAtlas(
    const SkinDigitSpriteSet &atlas, NumericGlyphAtlasBudget budget) noexcept {
  return validateNumericGlyphAtlasImpl(atlas, nullptr, budget);
}

NumericGlyphAtlasError validateNumericGlyphAtlas(
    const SkinDigitSpriteSet &atlas, NumericGlyphAtlasKind kind,
    NumericGlyphAtlasBudget budget) noexcept {
  return validateNumericGlyphAtlasImpl(atlas, &kind, budget);
}

NumericGlyphAtlasError validateNumericGlyphFormat(
    const NumericGlyphFormat &format, NumericGlyphAtlasKind kind,
    int glyphsPerAnimationFrame) noexcept {
  if (!validKind(kind)) {
    return NumericGlyphAtlasError::InvalidKind;
  }
  if (!validPadding(format.zeroPadding)) {
    return NumericGlyphAtlasError::InvalidPadding;
  }
  if (!finiteOffsets(format.perDigitOffsets) ||
      (kind == NumericGlyphAtlasKind::Float && !std::isfinite(format.gain))) {
    return NumericGlyphAtlasError::NonFiniteFormat;
  }
  if (kind == NumericGlyphAtlasKind::Number) {
    if (format.integerDigits < 0 ||
        format.integerDigits > NumericGlyphAtlasPolicy::maxNumberDigits ||
        format.fractionalDigits != 0 || format.signVisible ||
        format.perDigitOffsets.size() >
            NumericGlyphAtlasPolicy::maxNumberDigitOffsets) {
      return NumericGlyphAtlasError::InvalidFormat;
    }
    if (format.zeroPadding == SkinZeroPaddingMode::AlternateZero &&
        glyphsPerAnimationFrame < 11) {
      return NumericGlyphAtlasError::InvalidGlyphSet;
    }
    return NumericGlyphAtlasError::None;
  }

  if (format.integerDigits < 0 ||
      format.integerDigits > NumericGlyphAtlasPolicy::maxFloatDigits ||
      format.fractionalDigits < 0 ||
      format.fractionalDigits > NumericGlyphAtlasPolicy::maxFloatDigits ||
      format.integerDigits > NumericGlyphAtlasPolicy::maxFloatDigits -
                                 format.fractionalDigits ||
      format.perDigitOffsets.size() >
          NumericGlyphAtlasPolicy::maxFloatDigitOffsets) {
    return NumericGlyphAtlasError::InvalidFormat;
  }
  if (format.signVisible && glyphsPerAnimationFrame != 13) {
    return NumericGlyphAtlasError::InvalidGlyphSet;
  }
  return NumericGlyphAtlasError::None;
}

NumericGlyphAtlasResult partitionNumericGlyphAtlas(
    const NumericGlyphAtlasRequest &request) {
  if (!validKind(request.kind)) {
    return failure(NumericGlyphAtlasError::InvalidKind);
  }
  if (!request.pinnedLoaderCompatibility && !finiteFormat(request.format)) {
    return failure(NumericGlyphAtlasError::NonFiniteFormat);
  }
  // SkinNumber allocates its current-image array directly from `keta`.
  // Preserve that negative-size load failure rather than normalizing it to
  // zero; positive type-5 values remain intentionally uncapped.
  if (request.pinnedLoaderCompatibility &&
      request.kind == NumericGlyphAtlasKind::Number &&
      request.format.integerDigits < 0) {
    return failure(NumericGlyphAtlasError::InvalidFormat);
  }
  if (!request.pinnedLoaderCompatibility && request.source.frames.empty()) {
    return failure(NumericGlyphAtlasError::EmptyFrames);
  }
  if (!request.pinnedLoaderCompatibility &&
      request.source.frames.size() > request.budget.remainingMaterializedFrames) {
    return failure(NumericGlyphAtlasError::InputLimitExceeded);
  }
  const auto padding = normalizePadding(request.format.zeroPadding);
  return request.kind == NumericGlyphAtlasKind::Number
             ? partitionNumber(request, padding)
             : partitionFloat(request, padding);
}

} // namespace skin

#endif // ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
