#pragma once

#include "LuaSkinTableDecoder.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace skin {

// Limits the source-neutral glyph representation before any renderer-owned
// texture or command allocations are involved.
struct NumericGlyphAtlasPolicy {
  // Numeric expansion consumes the same model-wide frame budget as decoding.
  static constexpr std::size_t maxMaterializedFrames =
      LuaSkinTableDecoderPolicy::maxMaterializedSpriteFrames;
  static constexpr int maxFloatDigits = 8;
  static constexpr std::size_t maxFloatDigitOffsets =
      static_cast<std::size_t>(maxFloatDigits + 2);
  // Number does not use FloatFormatter. Keep a separate finite presentation
  // bound instead of applying FloatFormatter's eight-digit rule to it.
  // Reuse the established model offset-table bound for Number's independent
  // digit/offset presentation, rather than importing FloatFormatter's limit.
  static constexpr int maxNumberDigits =
      static_cast<int>(LuaSkinTableDecoderPolicy::maxOffsets);
  static constexpr std::size_t maxNumberDigitOffsets =
      LuaSkinTableDecoderPolicy::maxOffsets;
};

struct NumericGlyphAtlasBudget {
  // Decoder integration passes the remaining model-wide budget after source
  // frames already materialized for the current model. The default supports
  // direct, source-neutral validation of one standalone atlas.
  std::size_t remainingMaterializedFrames =
      NumericGlyphAtlasPolicy::maxMaterializedFrames;
};

enum class NumericGlyphAtlasKind : std::uint8_t { Number, Float };

enum class NumericGlyphAtlasError : std::uint8_t {
  None,
  InvalidKind,
  NonFiniteFormat,
  InvalidGlyphSet,
  EmptyFrames,
  UnsupportedGlyphLayout,
  InvalidPadding,
  InputLimitExceeded,
  OutputLimitExceeded,
  UnequalAnimationFrames,
  ArithmeticOverflow,
};

struct NumericGlyphFormatRequest {
  int integerDigits = 0;
  int fractionalDigits = 0;
  // Float and Number-24 use JsonSkin.Value.zeropadding. Number-10 uses the
  // separate JsonSkin.Value.padding field; Number-11 always forces mode 2.
  int zeroPadding = 0;
  int numberPadding = 0;
  bool signVisible = false;
  double gain = 1.0;
  std::vector<SkinDigitOffset> perDigitOffsets;
};

struct NumericGlyphFormat {
  int integerDigits = 0;
  int fractionalDigits = 0;
  SkinZeroPaddingMode zeroPadding = SkinZeroPaddingMode::None;
  bool signVisible = false;
  double gain = 1.0;
  std::vector<SkinDigitOffset> perDigitOffsets;
};

struct NumericGlyphAtlasRequest {
  NumericGlyphAtlasKind kind = NumericGlyphAtlasKind::Number;
  SkinSpriteFrames source;
  NumericGlyphFormatRequest format;
  NumericGlyphAtlasBudget budget;
};

struct NumericGlyphAtlas {
  SkinDigitSpriteSet digits;
  NumericGlyphFormat format;
};

struct NumericGlyphAtlasResult {
  std::optional<NumericGlyphAtlas> atlas;
  NumericGlyphAtlasError error = NumericGlyphAtlasError::None;
};

// Validates a normalized sprite set independently so callers that receive a
// model from a different source format cannot bypass animation-frame bounds.
NumericGlyphAtlasError validateNumericGlyphAtlas(
    const SkinDigitSpriteSet &atlas,
    NumericGlyphAtlasBudget budget = {}) noexcept;
NumericGlyphAtlasError validateNumericGlyphAtlas(
    const SkinDigitSpriteSet &atlas, NumericGlyphAtlasKind kind,
    NumericGlyphAtlasBudget budget = {}) noexcept;

// Partitions a row-major source atlas into the exact numeric glyph sets used
// by Beatoraja's Number and Float loaders. It is pure and owns no resources.
NumericGlyphAtlasResult partitionNumericGlyphAtlas(
    const NumericGlyphAtlasRequest &request);

} // namespace skin
