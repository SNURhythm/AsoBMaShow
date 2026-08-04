#include "skin/beatoraja/SkinTextGraphNormalization.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
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

SkinFontResource font(std::uint32_t id, std::string name = "main") {
  return {.id = id,
          .authoredName = std::move(name),
          .virtualPath = "font/main.ttf",
          .type = 0,
          .fallbacks = {{.virtualPath = "font/fallback.fnt", .type = 1},
                        {.virtualPath = "font/fallback.ttf", .type = 0}},
          .authoredOrdinal = 4};
}

std::vector<SkinFontResource> oneFont() { return {font(7)}; }

SkinTextNormalizationInput validTextInput() {
  return {.fontName = "main",
          .value = SkinStringPropertyId{11},
          .writer = SkinStringWriterId{12},
          .literal = "literal \xF0\x9F\x99\x82",
          .pointSize = 36,
          .alignment = 2,
          .wrapping = true,
          .overflow = SkinTextOverflow::Shrink,
          .outlineRgba = {1, 2, 3, 4},
          .outlineWidth = 1.25,
          .shadowRgba = {5, 6, 7, 8},
          .shadowOffsetX = -3.5,
          .shadowOffsetY = 4.5,
          .shadowSmoothness = 0.75,
          .writerWasExplicit = false,
          .authoredEditable = true};
}

SkinSpriteFrames sprite() {
  return {.resource = 101,
          .frames = {{.x = 10,
                      .y = 20,
                      .w = 30,
                      .h = 40,
                      .gridColumn = 1,
                      .gridRow = 2,
                      .gridColumns = 3,
                      .gridRows = 4},
                     {.x = 50,
                      .y = 60,
                      .w = 70,
                      .h = 80,
                      .gridColumn = 2,
                      .gridRow = 3,
                      .gridColumns = 3,
                      .gridRows = 4}},
          .cycleMillis = 120,
          .timer = SkinTimerPropertyId{9}};
}

SkinGraphNormalizationInput validGraphInput() {
  return {.fill = sprite(),
          .explicitRate = SkinFloatPropertyId{21},
          .implicitRate = SkinFloatPropertyId{22},
          .integerRange =
              std::optional<SkinSliderObject::IntegerRangeSource>{
                  SkinSliderObject::IntegerRangeSource{
                      .value = SkinIntegerPropertyId{23},
                      .minimum = -50,
                      .maximum = 150}},
          .isRefNum = false,
          .type = 0,
          .direction = 1};
}

void testTextRetainsPinnedFieldsAndResolvesFontIdentity() {
  std::vector<SkinFontResource> fonts{font(7)};
  const auto input = validTextInput();
  const auto result = normalizeSkinText(input, fonts);

  expect(result.text.has_value(), "valid text normalizes");
  expect(result.error == SkinTextGraphNormalizationError::None,
         "valid text has no error");
  expect(fonts.front().fallbacks.size() == 2 &&
             fonts.front().fallbacks[0].virtualPath == "font/fallback.fnt" &&
             fonts.front().fallbacks[0].type == 1,
         "font fallback metadata remains attached to the resolved resource");
  if (!result.text) {
    return;
  }
  const auto &text = *result.text;
  expect(text.font == 7, "text stores the resolved font resource identity");
  expect(text.value == SkinStringPropertyId{11},
         "text retains the typed property alongside literal text");
  expect(text.writer == SkinStringWriterId{12},
         "text retains the typed writer alongside literal text");
  expect(text.literal == input.literal, "text preserves valid UTF-8 literal");
  expect(text.pointSize == 36 && text.alignment == 2 && text.wrapping,
         "text preserves point size, alignment, and wrapping");
  expect(text.overflow == static_cast<int>(SkinTextOverflow::Shrink),
         "text preserves pinned overflow mode");
  expect(text.outlineRgba == input.outlineRgba &&
             text.outlineWidth == input.outlineWidth &&
             text.shadowRgba == input.shadowRgba &&
             text.shadowOffsetX == input.shadowOffsetX &&
             text.shadowOffsetY == input.shadowOffsetY &&
             text.shadowSmoothness == input.shadowSmoothness && text.editable,
         "text preserves outline, shadow, and editability fields");
}

void testTextComputesPinnedEffectiveEditability() {
  const auto fonts = oneFont();

  auto explicitWriter = validTextInput();
  explicitWriter.writerWasExplicit = true;
  explicitWriter.authoredEditable = false;
  const auto explicitResult = normalizeSkinText(explicitWriter, fonts);
  expect(explicitResult.text && !explicitResult.text->editable,
         "explicit event writer does not implicitly make text editable");

  auto fallbackWriter = validTextInput();
  fallbackWriter.writerWasExplicit = false;
  fallbackWriter.authoredEditable = false;
  const auto fallbackResult = normalizeSkinText(fallbackWriter, fonts);
  expect(fallbackResult.text && fallbackResult.text->editable,
         "ref fallback writer makes text editable when authored flag is false");

  auto authoredEditable = validTextInput();
  authoredEditable.writerWasExplicit = true;
  authoredEditable.authoredEditable = true;
  const auto authoredResult = normalizeSkinText(authoredEditable, fonts);
  expect(authoredResult.text && authoredResult.text->editable,
         "authored editable true remains effective with an explicit writer");
}

void testTextAcceptsPinnedNullFallbackPlaceholders() {
  auto withPlaceholder = font(7);
  withPlaceholder.fallbacks.insert(withPlaceholder.fallbacks.begin(),
                                   SkinFontFallbackResource{});
  const std::vector<SkinFontResource> fonts{withPlaceholder};
  const auto result = normalizeSkinText(validTextInput(), fonts);
  expect(result.text.has_value(),
         "empty fallback metadata mirrors pinned null fallback placeholder");
  expect(withPlaceholder.fallbacks.front().virtualPath.empty(),
         "normalization does not mutate font fallback metadata authority");
}

void testTextRejectsAmbiguousOrUnsafeInputs() {
  const auto input = validTextInput();
  const std::vector<SkinFontResource> duplicate{font(7), font(8)};
  const auto duplicateResult = normalizeSkinText(input, duplicate);
  expect(
      !duplicateResult.text &&
          duplicateResult.error ==
              SkinTextGraphNormalizationError::AmbiguousFont,
      "duplicate font identities fail closed instead of selecting first match");

  auto missingFont = validTextInput();
  missingFont.fontName = "missing";
  const auto missingFonts = oneFont();
  const auto missingResult = normalizeSkinText(missingFont, missingFonts);
  expect(!missingResult.text &&
             missingResult.error ==
                 SkinTextGraphNormalizationError::MissingFont,
         "missing font fails closed");

  auto malformed = validTextInput();
  malformed.literal = std::string("\xf0\x28\x8c\x28", 4);
  const auto malformedFonts = oneFont();
  const auto malformedResult = normalizeSkinText(malformed, malformedFonts);
  expect(!malformedResult.text &&
             malformedResult.error ==
                 SkinTextGraphNormalizationError::InvalidUtf8,
         "malformed literal UTF-8 fails closed");

  auto badStyle = validTextInput();
  badStyle.outlineWidth = std::numeric_limits<double>::infinity();
  const auto badStyleFonts = oneFont();
  const auto badStyleResult = normalizeSkinText(badStyle, badStyleFonts);
  expect(!badStyleResult.text &&
             badStyleResult.error ==
                 SkinTextGraphNormalizationError::InvalidTextStyle,
         "non-finite text style values fail closed");

  auto badLayout = validTextInput();
  badLayout.pointSize = 0;
  const auto badLayoutFonts = oneFont();
  const auto badLayoutResult = normalizeSkinText(badLayout, badLayoutFonts);
  expect(!badLayoutResult.text &&
             badLayoutResult.error ==
                 SkinTextGraphNormalizationError::InvalidTextLayout,
         "invalid point size fails closed");
}

void testGraphUsesPinnedSourcePrecedenceAndNormalizesDirection() {
  auto input = validGraphInput();
  const auto explicitResult = normalizeSkinGraph(input);
  expect(explicitResult.graph.has_value(), "explicit-rate graph normalizes");
  if (explicitResult.graph) {
    const auto *rate =
        std::get_if<SkinFloatPropertyId>(&explicitResult.graph->value);
    expect(rate && *rate == SkinFloatPropertyId{21},
           "explicit float property wins even when integer range is present");
    expect(explicitResult.graph->direction == 1,
           "pinned direction one remains downward");
    expect(
        explicitResult.graph->fill.frames.size() == input.fill.frames.size() &&
            explicitResult.graph->fill.frames[0].x == input.fill.frames[0].x &&
            explicitResult.graph->fill.frames[1].gridRows ==
                input.fill.frames[1].gridRows &&
            explicitResult.graph->fill.resource == input.fill.resource &&
            explicitResult.graph->fill.cycleMillis == input.fill.cycleMillis &&
            explicitResult.graph->fill.timer == input.fill.timer,
        "graph preserves resource, frames, cycle, and timer");
  }

  input.explicitRate.reset();
  input.isRefNum = true;
  input.direction = 99;
  const auto integerResult = normalizeSkinGraph(input);
  expect(integerResult.graph.has_value(), "integer-range graph normalizes");
  if (integerResult.graph) {
    const auto *range = std::get_if<SkinSliderObject::IntegerRangeSource>(
        &integerResult.graph->value);
    expect(range && range->value == SkinIntegerPropertyId{23} &&
               range->minimum == -50 && range->maximum == 150,
           "isRefNum selects integer source with min/max");
    expect(integerResult.graph->direction == 0,
           "every non-one upstream direction normalizes rightward");
  }

  input.isRefNum = false;
  input.direction = -44;
  const auto implicitResult = normalizeSkinGraph(input);
  expect(implicitResult.graph.has_value(), "implicit-rate graph normalizes");
  if (implicitResult.graph) {
    const auto *rate =
        std::get_if<SkinFloatPropertyId>(&implicitResult.graph->value);
    expect(rate && *rate == SkinFloatPropertyId{22},
           "non-isRefNum graph uses typed implicit rate source");
    expect(implicitResult.graph->direction == 0,
           "negative upstream direction is rightward by pinned semantics");
  }
}

void testGraphRejectsInvalidDependencies() {
  auto input = validGraphInput();
  input.fill.frames.clear();
  const auto missingFrames = normalizeSkinGraph(input);
  expect(!missingFrames.graph &&
             missingFrames.error ==
                 SkinTextGraphNormalizationError::InvalidGraphSprite,
         "empty graph frame list fails closed");

  input = validGraphInput();
  input.explicitRate.reset();
  input.isRefNum = true;
  input.integerRange->maximum = input.integerRange->minimum - 1;
  const auto descendingRange = normalizeSkinGraph(input);
  const auto *descending =
      descendingRange.graph ? std::get_if<SkinSliderObject::IntegerRangeSource>(
                                  &descendingRange.graph->value)
                            : nullptr;
  expect(descending && descending->minimum == -50 && descending->maximum == -51,
         "descending integer graph ranges preserve pinned RateProperty "
         "semantics");

  input = validGraphInput();
  input.explicitRate.reset();
  input.isRefNum = true;
  input.integerRange->maximum = input.integerRange->minimum;
  const auto emptyRange = normalizeSkinGraph(input);
  expect(!emptyRange.graph &&
             emptyRange.error ==
                 SkinTextGraphNormalizationError::InvalidGraphRange,
         "zero-span integer graph range fails closed before runtime");

  input = validGraphInput();
  input.explicitRate.reset();
  input.isRefNum = true;
  input.integerRange->minimum = std::numeric_limits<int>::min();
  input.integerRange->maximum = std::numeric_limits<int>::max();
  const auto oversizedRange = normalizeSkinGraph(input);
  expect(!oversizedRange.graph &&
             oversizedRange.error ==
                 SkinTextGraphNormalizationError::InvalidGraphRange,
         "integer graph spans above Java INT_MAX fail during normalization");

  input = validGraphInput();
  input.explicitRate.reset();
  input.isRefNum = false;
  input.implicitRate = SkinFloatPropertyId{};
  const auto missingRate = normalizeSkinGraph(input);
  const auto *implicit =
      missingRate.graph
          ? std::get_if<SkinFloatPropertyId>(&missingRate.graph->value)
          : nullptr;
  expect(implicit && !*implicit,
         "invalid implicit graph binding preserves a typed zero sentinel");
}

void testDistributionGraphsAreExplicitlyUnsupported() {
  for (const int type : {-1, -2, -28}) {
    auto input = validGraphInput();
    input.type = type;
    input.fill.frames.clear();
    input.explicitRate = SkinFloatPropertyId{};
    const auto result = normalizeSkinGraph(input);
    expect(
        !result.graph &&
            result.error ==
                SkinTextGraphNormalizationError::UnsupportedDistributionGraph,
        "negative graph type is diagnosed before regular graph precedence");
  }
}

} // namespace

int main() {
  testTextRetainsPinnedFieldsAndResolvesFontIdentity();
  testTextComputesPinnedEffectiveEditability();
  testTextAcceptsPinnedNullFallbackPlaceholders();
  testTextRejectsAmbiguousOrUnsafeInputs();
  testGraphUsesPinnedSourcePrecedenceAndNormalizesDirection();
  testGraphRejectsInvalidDependencies();
  testDistributionGraphsAreExplicitlyUnsupported();
  return failures == 0 ? 0 : 1;
}
