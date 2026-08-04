#include "skin/beatoraja/NumericGlyphAtlas.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

namespace {

using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::vector<SkinSourceRect> frames(int count) {
  std::vector<SkinSourceRect> result;
  result.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    result.push_back({.x = i, .y = -i, .w = 10 + i, .h = 20 + i});
  }
  return result;
}

NumericGlyphAtlasRequest request(NumericGlyphAtlasKind kind, int frameCount) {
  NumericGlyphAtlasRequest value;
  value.kind = kind;
  value.source.resource = 41;
  value.source.frames = frames(frameCount);
  value.source.cycleMillis = 250;
  value.format.integerDigits = 3;
  value.format.fractionalDigits = 2;
  value.format.zeroPadding = 1;
  value.format.numberPadding = 1;
  value.format.signVisible = true;
  value.format.gain = 1.25;
  value.format.perDigitOffsets = {
      {.x = 1.0, .y = 2.0, .width = 3.0, .height = 4.0},
      {.x = 5.0, .y = 6.0, .width = 7.0, .height = 8.0},
  };
  return value;
}

const NumericGlyphAtlas &mustSucceed(const NumericGlyphAtlasResult &result,
                                     std::string_view message) {
  expect(result.atlas.has_value(), message);
  if (result.atlas) {
    return *result.atlas;
  }
  std::abort();
}

void numberLayoutsPreserveRowMajorFrames() {
  {
    auto input = request(NumericGlyphAtlasKind::Number, 20);
    input.format.zeroPadding = 2;
    input.format.numberPadding = 1;
    const auto atlas = mustSucceed(partitionNumericGlyphAtlas(input),
        "10-glyph number atlas partitions");
    expect(!atlas.digits.negative.has_value(),
           "10-glyph number atlas has no negative set");
    expect(atlas.digits.glyphsPerAnimationFrame == 10,
           "10-glyph number layout width");
    expect(atlas.digits.positive.frames.size() == 20,
           "10-glyph number keeps both animation frames");
    expect(atlas.digits.positive.frames[10].x == 10,
           "10-glyph number frame ordering is row major");
    expect(atlas.format.zeroPadding == SkinZeroPaddingMode::Zero,
           "10-glyph number preserves authored padding");
  }
  {
    const auto atlas = mustSucceed(
        partitionNumericGlyphAtlas(request(NumericGlyphAtlasKind::Number, 22)),
        "11-glyph number atlas partitions");
    expect(atlas.digits.glyphsPerAnimationFrame == 11,
           "11-glyph number layout width");
    expect(atlas.digits.positive.frames[11].x == 11,
           "11-glyph number retains the second row order");
    expect(atlas.format.zeroPadding == SkinZeroPaddingMode::AlternateZero,
           "11-glyph number forces alternate-zero padding");
  }
  {
    const auto atlas = mustSucceed(
        partitionNumericGlyphAtlas(request(NumericGlyphAtlasKind::Number, 48)),
        "24-glyph number atlas partitions");
    expect(atlas.digits.negative.has_value(),
           "24-glyph number atlas has negative set");
    expect(atlas.digits.glyphsPerAnimationFrame == 12,
           "24-glyph number layout width");
    expect(atlas.digits.positive.frames.size() == 24 &&
               atlas.digits.negative->frames.size() == 24,
           "24-glyph number keeps two complete animation frame sets");
    expect(atlas.digits.positive.frames[12].x == 24 &&
               atlas.digits.negative->frames[12].x == 36,
           "24-glyph number partitions each row-major animation frame");
  }
  {
    auto input = request(NumericGlyphAtlasKind::Number, 120);
    input.format.integerDigits = 12;
    const auto atlas = mustSucceed(partitionNumericGlyphAtlas(input),
                                   "ambiguous number atlas partitions");
    expect(atlas.digits.glyphsPerAnimationFrame == 12 &&
               atlas.digits.negative.has_value(),
           "number prefers 24 over its simultaneous 10-glyph layout");
    expect(atlas.format.integerDigits == 12,
           "number digit count does not inherit float's eight-digit limit");
  }
  {
    auto input = request(NumericGlyphAtlasKind::Number, 20);
    input.format.perDigitOffsets.resize(12);
    input.format.perDigitOffsets[11] =
        {.x = 11.0, .y = 12.0, .width = 13.0, .height = 14.0};
    const auto atlas = mustSucceed(partitionNumericGlyphAtlas(input),
                                   "number supports its own bounded offsets");
    expect(atlas.format.perDigitOffsets.size() == 12 &&
               atlas.format.perDigitOffsets[11].height == 14.0,
           "number offsets are not constrained by float sign/decimal slots");
  }
}

void floatLayoutsMatchPinnedGlyphConventions() {
  struct LayoutCase {
    int sourceCount;
    int glyphs;
    bool hasNegative;
    bool signVisible;
  };
  for (const LayoutCase layout : {
           LayoutCase{11, 12, false, false}, LayoutCase{12, 12, false, false},
           LayoutCase{24, 12, true, false},
           LayoutCase{44, 12, true, false}, LayoutCase{48, 12, true, false},
           LayoutCase{52, 13, true, true},
       }) {
    auto input = request(NumericGlyphAtlasKind::Float, layout.sourceCount);
    const auto atlas = mustSucceed(partitionNumericGlyphAtlas(input),
                                   "float atlas layout partitions");
    expect(atlas.digits.glyphsPerAnimationFrame == layout.glyphs,
           "float layout glyph count");
    expect(atlas.digits.negative.has_value() == layout.hasNegative,
           "float layout negative set convention");
    expect(atlas.format.signVisible == layout.signVisible,
           "only 26-layout preserves the authored sign");
    expect(atlas.format.gain == 1.25,
           "float gain survives normalization");
    expect(atlas.format.perDigitOffsets.size() == 2 &&
               atlas.format.perDigitOffsets[0].x == 1.0 &&
               atlas.format.perDigitOffsets[0].y == 2.0 &&
               atlas.format.perDigitOffsets[0].width == 3.0 &&
               atlas.format.perDigitOffsets[0].height == 4.0 &&
               atlas.format.perDigitOffsets[1].x == 5.0 &&
               atlas.format.perDigitOffsets[1].y == 6.0 &&
               atlas.format.perDigitOffsets[1].width == 7.0 &&
               atlas.format.perDigitOffsets[1].height == 8.0,
           "partial per-digit x/y/w/h offsets survive normalization");
  }

  const auto eleven = mustSucceed(
      partitionNumericGlyphAtlas(request(NumericGlyphAtlasKind::Float, 33)),
      "11-glyph float atlas partitions");
  expect(eleven.digits.positive.frames[0].x == 0 &&
             eleven.digits.positive.frames[10].x == 0 &&
             eleven.digits.positive.frames[11].x == 10,
         "11 layout repeats zero as reverse-zero then decimal");
  expect(eleven.digits.positive.frames[12].x == 11,
         "11 layout preserves the next animation frame row order");

  const auto twentyTwo = mustSucceed(
      partitionNumericGlyphAtlas(request(NumericGlyphAtlasKind::Float, 44)),
      "22-glyph float atlas partitions");
  expect(twentyTwo.digits.positive.frames[10].x == 0 &&
             twentyTwo.digits.negative->frames[10].x == 11 &&
             twentyTwo.digits.positive.frames[11].x == 10 &&
             twentyTwo.digits.negative->frames[11].x == 21,
         "22 layout repeats each signed zero then preserves decimal");
  expect(twentyTwo.digits.positive.frames[12].x == 22 &&
             twentyTwo.digits.negative->frames[12].x == 33,
             "22 layout partitions multiple animation frames row major");

  const auto precedence = mustSucceed(
      partitionNumericGlyphAtlas(request(NumericGlyphAtlasKind::Float, 3432)),
      "ambiguous float atlas partitions");
  expect(precedence.digits.glyphsPerAnimationFrame == 13 &&
             precedence.digits.negative.has_value() &&
             precedence.format.signVisible,
         "float prefers 26 over simultaneous 24, 22, 12, and 11 layouts");
}

void floatFormatNormalizesDigitsAndPadding() {
  {
    auto input = request(NumericGlyphAtlasKind::Float, 12);
    input.format.integerDigits = -4;
    input.format.fractionalDigits = -1;
    input.format.zeroPadding = 0;
    const auto atlas = mustSucceed(partitionNumericGlyphAtlas(input),
                                   "negative digit counts normalize");
    expect(atlas.format.integerDigits == 0 && atlas.format.fractionalDigits == 0,
           "negative digit counts normalize to zero");
    expect(atlas.format.zeroPadding == SkinZeroPaddingMode::None,
           "padding mode none normalizes");
  }
  {
    auto input = request(NumericGlyphAtlasKind::Float, 12);
    input.format.integerDigits = 7;
    input.format.fractionalDigits = 5;
    input.format.zeroPadding = 2;
    const auto atlas = mustSucceed(partitionNumericGlyphAtlas(input),
                                   "oversized float digits normalize");
    expect(atlas.format.integerDigits == 3 && atlas.format.fractionalDigits == 5,
           "float fraction count has priority within eight digits");
    expect(atlas.format.zeroPadding == SkinZeroPaddingMode::AlternateZero,
           "padding mode alternate zero normalizes");
  }
  {
    auto input = request(NumericGlyphAtlasKind::Float, 12);
    input.format.integerDigits = 3;
    input.format.fractionalDigits = 10;
    const auto atlas = mustSucceed(partitionNumericGlyphAtlas(input),
                                   "oversized fraction count normalizes");
    expect(atlas.format.integerDigits == 0 && atlas.format.fractionalDigits == 8,
           "float fraction is bounded to eight digits");
  }
  {
    auto input = request(NumericGlyphAtlasKind::Float, 12);
    input.format.zeroPadding = -100;
    const auto negative = mustSucceed(partitionNumericGlyphAtlas(input),
                                      "negative float padding normalizes");
    input.format.zeroPadding = 100;
    const auto large = mustSucceed(partitionNumericGlyphAtlas(input),
                                   "large float padding normalizes");
    expect(negative.format.zeroPadding == SkinZeroPaddingMode::None &&
               large.format.zeroPadding == SkinZeroPaddingMode::AlternateZero,
           "float padding clamps to the pinned three modes");
  }
}

void rejectsMalformedOrUnboundedAtlases() {
  for (const auto kind : {NumericGlyphAtlasKind::Number,
                          NumericGlyphAtlasKind::Float}) {
    auto input = request(kind, 0);
    const auto result = partitionNumericGlyphAtlas(input);
    expect(result.error == NumericGlyphAtlasError::EmptyFrames,
           "zero glyph source rejects without output");
    expect(!result.atlas, "zero glyph source has no partial output");
  }
  {
    auto input = request(NumericGlyphAtlasKind::Float, 13);
    const auto result = partitionNumericGlyphAtlas(input);
    expect(result.error == NumericGlyphAtlasError::UnsupportedGlyphLayout,
           "non-divisible float glyph source rejects");
  }
  {
    auto input = request(NumericGlyphAtlasKind::Float, 12);
    input.source.frames.resize(
        LuaSkinTableDecoderPolicy::maxMaterializedSpriteFrames + 1);
    const auto result = partitionNumericGlyphAtlas(input);
    expect(result.error == NumericGlyphAtlasError::InputLimitExceeded,
           "input frame hard limit rejects before partitioning");
  }
  {
    const auto atlas = mustSucceed(
        partitionNumericGlyphAtlas(request(NumericGlyphAtlasKind::Float,
                                           22 * 8332)),
        "largest selected Float-22 atlas below budget partitions before "
        "8333 selects Float-26 by precedence");
    expect(atlas.digits.positive.frames.size() +
                   atlas.digits.negative->frames.size() ==
               199'968,
           "Float-22 output budget applies across both signed sets");
  }
  {
    auto input = request(NumericGlyphAtlasKind::Float, 22 * 8334);
    const auto result = partitionNumericGlyphAtlas(input);
    expect(result.error == NumericGlyphAtlasError::OutputLimitExceeded,
           "smallest selected Float-22 atlas above budget rejects before "
           "allocation");
  }
  {
    const auto atlas = mustSucceed(
        partitionNumericGlyphAtlas(request(NumericGlyphAtlasKind::Number,
                                           200'000)),
        "exact model frame budget partitions for Number-10");
    expect(atlas.digits.positive.frames.size() == 200'000,
           "Number-10 may consume exactly the central model frame budget");
  }
  {
    SkinDigitSpriteSet invalid;
    invalid.glyphsPerAnimationFrame = 12;
    invalid.positive.frames = frames(24);
    SkinSpriteFrames negative;
    negative.frames = frames(12);
    invalid.negative = std::move(negative);
    expect(validateNumericGlyphAtlas(invalid) ==
               NumericGlyphAtlasError::UnequalAnimationFrames,
           "unequal positive and negative animation frame counts reject");
  }
  {
    SkinDigitSpriteSet invalid;
    invalid.glyphsPerAnimationFrame = std::numeric_limits<int>::max();
    invalid.positive.frames = frames(1);
    expect(validateNumericGlyphAtlas(invalid) ==
               NumericGlyphAtlasError::ArithmeticOverflow,
           "glyph count arithmetic overflow rejects");
  }
}

void rejectsInvalidKindsAndNonFiniteFormats() {
  {
    auto input = request(static_cast<NumericGlyphAtlasKind>(0xff), 12);
    const auto result = partitionNumericGlyphAtlas(input);
    expect(result.error == NumericGlyphAtlasError::InvalidKind,
           "partition rejects an invalid public kind enum");
  }
  {
    SkinDigitSpriteSet digits;
    expect(validateNumericGlyphAtlas(
               digits, static_cast<NumericGlyphAtlasKind>(0xff)) ==
               NumericGlyphAtlasError::InvalidKind,
           "validation rejects an invalid public kind before atlas shape");
  }
  for (const double gain : {std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity()}) {
    auto input = request(NumericGlyphAtlasKind::Float, 12);
    input.format.gain = gain;
    const auto result = partitionNumericGlyphAtlas(input);
    expect(result.error == NumericGlyphAtlasError::NonFiniteFormat,
           "non-finite float gain rejects");
  }

  constexpr std::array offsetMembers{
      &SkinDigitOffset::x, &SkinDigitOffset::y, &SkinDigitOffset::width,
      &SkinDigitOffset::height};
  for (const auto member : offsetMembers) {
    auto input = request(NumericGlyphAtlasKind::Number, 10);
    input.format.perDigitOffsets.front().*member =
        std::numeric_limits<double>::quiet_NaN();
    const auto result = partitionNumericGlyphAtlas(input);
    expect(result.error == NumericGlyphAtlasError::NonFiniteFormat,
           "non-finite per-digit offset component rejects");
  }

  auto finiteBoundary = request(NumericGlyphAtlasKind::Float, 12);
  finiteBoundary.format.gain = std::numeric_limits<double>::max();
  finiteBoundary.format.perDigitOffsets.front() = {
      .x = std::numeric_limits<double>::lowest(),
      .y = std::numeric_limits<double>::max(),
      .width = -0.0,
      .height = 0.0,
  };
  expect(partitionNumericGlyphAtlas(finiteBoundary).atlas.has_value(),
         "finite double boundaries remain accepted");
}

void validatesKindSpecificNormalizedGlyphSets() {
  SkinDigitSpriteSet number;
  number.glyphsPerAnimationFrame = 13;
  number.positive.frames = frames(13);
  expect(validateNumericGlyphAtlas(number, NumericGlyphAtlasKind::Number) ==
             NumericGlyphAtlasError::InvalidGlyphSet,
         "Number validation rejects Float-only 13-glyph sets");

  SkinDigitSpriteSet floating;
  floating.glyphsPerAnimationFrame = 10;
  floating.positive.frames = frames(10);
  expect(validateNumericGlyphAtlas(floating, NumericGlyphAtlasKind::Float) ==
             NumericGlyphAtlasError::InvalidGlyphSet,
         "Float validation rejects Number-only 10-glyph sets");

  SkinDigitSpriteSet numberBoundary;
  numberBoundary.glyphsPerAnimationFrame = 12;
  numberBoundary.positive.frames = frames(12);
  numberBoundary.negative = SkinSpriteFrames{.frames = frames(12)};
  expect(validateNumericGlyphAtlas(numberBoundary,
                                   NumericGlyphAtlasKind::Number) ==
             NumericGlyphAtlasError::None,
         "Number normalized 12-glyph boundary remains valid");

  SkinDigitSpriteSet floatBoundary;
  floatBoundary.glyphsPerAnimationFrame = 13;
  floatBoundary.positive.frames = frames(13);
  floatBoundary.negative = SkinSpriteFrames{.frames = frames(13)};
  expect(validateNumericGlyphAtlas(floatBoundary,
                                   NumericGlyphAtlasKind::Float) ==
             NumericGlyphAtlasError::None,
         "Float normalized 13-glyph boundary remains valid");
}

} // namespace

int main() {
  numberLayoutsPreserveRowMajorFrames();
  floatLayoutsMatchPinnedGlyphConventions();
  floatFormatNormalizesDigitsAndPadding();
  rejectsMalformedOrUnboundedAtlases();
  rejectsInvalidKindsAndNonFiniteFormats();
  validatesKindSpecificNormalizedGlyphSets();
  return failures == 0 ? 0 : 1;
}
