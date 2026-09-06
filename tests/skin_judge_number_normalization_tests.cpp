#include "skin/beatoraja/SkinJudgeNumberNormalization.h"

#include <cmath>
#include <cstdint>
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

SkinJudgeNumberNormalizationInput inputFor(std::size_t frameCount) {
  SkinJudgeNumberNormalizationInput input;
  input.source.resource = 47;
  input.source.cycleMillis = 180;
  input.source.timer = SkinTimerPropertyId{19};
  input.source.frames.reserve(frameCount);
  for (std::size_t index = 0; index < frameCount; ++index) {
    input.source.frames.push_back(
        {.x = static_cast<int>(index), .y = -static_cast<int>(index),
         .w = 10, .h = 20});
  }
  input.value = SkinIntegerPropertyId{73};
  input.digitCount = 3;
  input.spacing = 8;
  input.offsets = {{.x = 1.0, .y = 2.0, .width = 3.0, .height = 4.0},
                   {.x = 5.0, .y = 6.0, .width = 7.0, .height = 8.0}};
  input.destination.frames = {
      {.timeMillis = 0, .x = 100.0, .y = 2.0, .width = 20.0, .height = 30.0},
      {.timeMillis = 50, .x = -12.0, .y = 3.0, .width = 8.0, .height = 9.0},
  };
  return input;
}

SkinJudgeNumberPresentation mustSucceed(
    const SkinJudgeNumberNormalizationResult &result, std::string_view message) {
  expect(result.number.has_value(), message);
  if (!result.number) {
    std::abort();
  }
  expect(result.error == SkinJudgeNumberNormalizationError::None,
         "successful normalization reports no error");
  return *result.number;
}

void testPinnedTenAndElevenGlyphLayouts() {
  struct LayoutCase {
    std::size_t sourceFrames;
    int glyphsPerRow;
    std::size_t retainedFrames;
    SkinZeroPaddingMode padding;
  };
  for (const LayoutCase test : {
           LayoutCase{10, 10, 10, SkinZeroPaddingMode::None},
           LayoutCase{11, 11, 11, SkinZeroPaddingMode::AlternateZero},
       }) {
    const auto normalized = mustSucceed(
        normalizeSkinJudgeNumber(inputFor(test.sourceFrames)),
        "pinned judge glyph layout normalizes");
    expect(normalized.number.digits.glyphsPerAnimationFrame == test.glyphsPerRow,
           "judge uses the pinned ten-or-eleven row width");
    expect(normalized.number.digits.positive.frames.size() ==
               test.retainedFrames,
           "judge retains exactly complete positive rows");
    expect(!normalized.number.digits.negative.has_value(),
           "ten and eleven judge layouts have no negative glyph partition");
    expect(normalized.number.zeroPadding == test.padding,
           "judge applies the pinned zero-padding mode");
  }
}

void testCompleteRowsStayRowMajor() {
  struct LayoutCase {
    std::size_t sourceFrames;
    int glyphsPerRow;
    std::size_t retainedFrames;
  };
  for (const LayoutCase test : {
           LayoutCase{20, 10, 20},
           LayoutCase{21, 11, 11},
           LayoutCase{22, 11, 22},
       }) {
    const auto normalized = mustSucceed(
        normalizeSkinJudgeNumber(inputFor(test.sourceFrames)),
        "complete or trailing judge rows normalize");
    expect(normalized.number.digits.glyphsPerAnimationFrame == test.glyphsPerRow,
           "twenty, twenty-one, and twenty-two select their pinned row width");
    expect(normalized.number.digits.positive.frames.size() == test.retainedFrames,
           "trailing frames beyond a full judge row are ignored");
    expect(normalized.number.digits.positive.frames.back().x ==
               static_cast<int>(test.retainedFrames - 1),
           "complete judge rows retain source row-major order");
  }

  const auto trailing = mustSucceed(normalizeSkinJudgeNumber(inputFor(12)),
                                    "twelve-frame trailing judge input normalizes");
  expect(trailing.number.digits.glyphsPerAnimationFrame == 11 &&
             trailing.number.digits.positive.frames.size() == 11 &&
             trailing.number.digits.positive.frames.back().x == 10,
         "a malformed twelve-frame tail retains only the first eleven-frame row");
}

void testJudgeNumberCopiesPinnedPresentationAndShiftsEveryDestination() {
  const auto normalized = mustSucceed(normalizeSkinJudgeNumber(inputFor(11)),
                                      "judge number presentation normalizes");
  expect(normalized.number.value == SkinIntegerPropertyId{73},
         "judge number uses ref as its typed integer binding");
  expect(normalized.number.digitCount == 3 && normalized.number.spacing == 8,
         "judge number preserves digit and spacing fields");
  expect(normalized.number.alignment == 2 &&
             normalized.number.relativeToJudgeImage,
         "judge number forces pinned centered relative placement");
  expect(normalized.number.perDigitOffsets.size() == 2 &&
             normalized.number.perDigitOffsets[1].width == 7.0,
         "judge number copies every authored digit offset");
  expect(normalized.destination.frames.size() == 2 &&
             normalized.destination.frames[0].x == 70.0 &&
             normalized.destination.frames[1].x == -24.0,
         "judge number shifts every destination x by half its digit width");
  expect(normalized.number.digits.positive.cycleMillis == 180 &&
             normalized.number.digits.positive.timer == SkinTimerPropertyId{19},
         "judge number preserves source animation timing");
}

void testJudgeShiftUsesPinnedIntegerTruncation() {
  auto input = inputFor(11);
  input.destination.frames = {{.timeMillis = 0,
                               .x = 100.0,
                               .y = 0.0,
                               .width = 9.0,
                               .height = 20.0}};
  const auto normalized = mustSucceed(normalizeSkinJudgeNumber(input),
                                      "odd judge geometry normalizes");
  expect(normalized.destination.frames.front().x == 87.0,
         "judge shift truncates odd width-times-digit products as Java ints");
}

void testUnsafeInputsFailClosed() {
  {
    auto input = inputFor(0);
    const auto result = normalizeSkinJudgeNumber(input);
    expect(!result.number.has_value() &&
               result.error == SkinJudgeNumberNormalizationError::EmptyFrames,
           "zero judge rows fail closed");
  }
  {
    auto input = inputFor(
        SkinJudgeNumberNormalizationPolicy::maxMaterializedFrames + 1);
    const auto result = normalizeSkinJudgeNumber(input);
    expect(!result.number.has_value() &&
               result.error == SkinJudgeNumberNormalizationError::FrameLimitExceeded,
           "judge frame budget fails closed before output allocation");
  }
  {
    auto input = inputFor(11);
    input.destination.frames.front().width =
        std::numeric_limits<double>::infinity();
    const auto result = normalizeSkinJudgeNumber(input);
    expect(!result.number.has_value() &&
               result.error == SkinJudgeNumberNormalizationError::NonFiniteGeometry,
           "non-finite judge destination geometry fails closed");
  }
  {
    auto input = inputFor(11);
    input.offsets.front().height = std::numeric_limits<double>::quiet_NaN();
    const auto result = normalizeSkinJudgeNumber(input);
    expect(!result.number.has_value() &&
               result.error == SkinJudgeNumberNormalizationError::NonFiniteGeometry,
           "non-finite judge digit offsets fail closed");
  }
  {
    auto input = inputFor(11);
    input.value = {};
    const auto result = normalizeSkinJudgeNumber(input);
    expect(result.number.has_value() &&
               result.error == SkinJudgeNumberNormalizationError::None &&
               !result.number->number.value,
           "judge number without a value ref keeps the null binding, which "
           "the renderer hides via the IntegerPropertyFactory sentinel");
  }
  {
    auto input = inputFor(11);
    input.digitCount = -1;
    const auto result = normalizeSkinJudgeNumber(input);
    expect(!result.number.has_value() &&
               result.error == SkinJudgeNumberNormalizationError::InvalidDigitCount,
           "negative judge digit counts fail closed");
  }
  {
    auto input = inputFor(11);
    input.digitCount =
        SkinJudgeNumberNormalizationPolicy::maxDigitCount + 1;
    const auto result = normalizeSkinJudgeNumber(input);
    expect(!result.number.has_value() &&
               result.error == SkinJudgeNumberNormalizationError::InvalidDigitCount,
           "over-policy judge digit counts fail closed");
  }
}

} // namespace

int main() {
  testPinnedTenAndElevenGlyphLayouts();
  testCompleteRowsStayRowMajor();
  testJudgeNumberCopiesPinnedPresentationAndShiftsEveryDestination();
  testJudgeShiftUsesPinnedIntegerTruncation();
  testUnsafeInputsFailClosed();
  return failures == 0 ? 0 : 1;
}
