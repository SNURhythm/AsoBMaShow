#include "skin/beatoraja/LuaSkinTableDecoder.h"
#include "skin/beatoraja/NumericGlyphAtlas.h"
#include "skin/beatoraja/SkinModelValidator.h"
#include "lua_skin_binding_test_support.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>
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

SkinSpriteFrames sprite(std::size_t count) {
  SkinSpriteFrames result;
  result.resource = 1;
  result.frames.resize(count);
  return result;
}

BeatorajaSkinModel numberModel(SkinDigitSpriteSet digits) {
  BeatorajaSkinModel model;
  model.header.type = 0;
  model.header.width = 1280;
  model.header.height = 720;
  SkinBuiltinPropertySelector source;
  source.value = 1;
  model.integerProperties.push_back(
      {.id = SkinIntegerPropertyId{1},
       .domain = SkinIntegerPropertyDomain::IntegerValue,
       .source = std::move(source)});
  model.resources.push_back(SkinImageResource{
      .id = 1, .authoredName = "atlas", .virtualPath = "atlas.png"});
  SkinNumberObject number;
  number.digits = std::move(digits);
  number.value = SkinIntegerPropertyId{1};
  model.objects.push_back({.id = 1,
                           .authoredName = "number",
                           .payload = std::move(number),
                           .critical = false});
  return model;
}

BeatorajaSkinModel floatModel(SkinDigitSpriteSet digits) {
  BeatorajaSkinModel model;
  model.header.type = 0;
  model.header.width = 1280;
  model.header.height = 720;
  SkinBuiltinPropertySelector source;
  source.value = 1;
  model.floatProperties.push_back(
      {.id = SkinFloatPropertyId{1},
       .domain = SkinFloatPropertyDomain::FloatValue,
       .source = std::move(source)});
  model.resources.push_back(SkinImageResource{
      .id = 1, .authoredName = "atlas", .virtualPath = "atlas.png"});
  SkinFloatObject floating;
  floating.digits = std::move(digits);
  floating.value = SkinFloatPropertyId{1};
  model.objects.push_back({.id = 1,
                           .authoredName = "float",
                           .payload = std::move(floating),
                           .critical = false});
  return model;
}

BeatorajaSkinModel
graphModel(SkinSliderObject::IntegerRangeSource integerRange) {
  BeatorajaSkinModel model;
  model.header.type = 0;
  model.header.width = 1280;
  model.header.height = 720;
  SkinBuiltinPropertySelector source;
  source.value = 1;
  model.integerProperties.push_back(
      {.id = integerRange.value,
       .domain = SkinIntegerPropertyDomain::IntegerValue,
       .source = std::move(source)});
  model.resources.push_back(SkinImageResource{
      .id = 1, .authoredName = "atlas", .virtualPath = "atlas.png"});
  SkinGraphObject graph;
  graph.fill = sprite(1);
  graph.value = integerRange;
  model.objects.push_back({.id = 1,
                           .authoredName = "graph",
                           .payload = std::move(graph),
                           .critical = false});
  return model;
}

void expectDisabled(SkinModelValidationResult result,
                    std::string_view message) {
  expect(result.model.has_value() && !result.criticalFailure &&
             result.model->disabledOptionalObjects ==
                 std::vector<SkinObjectId>{1} &&
             !result.diagnostics.empty() &&
             result.diagnostics.front().code ==
                 "skin_lua_model_optional_object_disabled",
         message);
}

SkinDigitSpriteSet validNumberDigits() {
  SkinDigitSpriteSet digits;
  digits.glyphsPerAnimationFrame = 10;
  digits.positive = sprite(10);
  return digits;
}

SkinDigitSpriteSet validFloatDigits() {
  SkinDigitSpriteSet digits;
  digits.glyphsPerAnimationFrame = 12;
  digits.positive = sprite(12);
  return digits;
}

SkinNumberObject &numberObject(BeatorajaSkinModel &model) {
  return std::get<SkinNumberObject>(model.objects.front().payload);
}

SkinFloatObject &floatObject(BeatorajaSkinModel &model) {
  return std::get<SkinFloatObject>(model.objects.front().payload);
}

void expectOptionalAndCriticalRejected(BeatorajaSkinModel model,
                                       std::string_view message) {
  auto optional = test_support::validateWithAuthoredBuiltins(model);
  expect(optional.model.has_value() && !optional.criticalFailure &&
             optional.model->disabledOptionalObjects ==
                 std::vector<SkinObjectId>{1},
         message);

  model.objects.front().critical = true;
  auto critical = test_support::validateWithAuthoredBuiltins(std::move(model));
  expect(!critical.model && critical.criticalFailure &&
             !critical.diagnostics.empty() &&
             critical.diagnostics.front().code ==
                 "skin_lua_model_critical_dependency_invalid",
         message);
}

void validatorDiagnosticsNameTheRejectedObject() {
  auto optionalModel = floatModel(validFloatDigits());
  floatObject(optionalModel).integerDigits = -1;
  const auto optional = test_support::validateWithAuthoredBuiltins(
      std::move(optionalModel));
  expect(optional.model && !optional.diagnostics.empty() &&
             optional.diagnostics.front().message.find("'float'") !=
                 std::string::npos,
         "optional dependency diagnostics identify their authored object");

  auto criticalModel = floatModel(validFloatDigits());
  floatObject(criticalModel).integerDigits = -1;
  criticalModel.objects.front().critical = true;
  const auto critical = test_support::validateWithAuthoredBuiltins(
      std::move(criticalModel));
  expect(!critical.model && !critical.diagnostics.empty() &&
             critical.diagnostics.front().message.find("'float'") !=
                 std::string::npos,
         "critical dependency diagnostics identify their authored object");
}

void validatorRejectsUnequalInvalidAndExcessDigitSets() {
  {
    SkinDigitSpriteSet digits;
    digits.glyphsPerAnimationFrame = 12;
    digits.positive = sprite(24);
    digits.negative = sprite(12);
    expectDisabled(test_support::validateWithAuthoredBuiltins(
                       numberModel(std::move(digits))),
                   "validator disables unequal signed animation-frame sets");
  }
  {
    SkinDigitSpriteSet digits;
    digits.glyphsPerAnimationFrame = 10;
    digits.positive = sprite(11);
    expectDisabled(test_support::validateWithAuthoredBuiltins(
                       numberModel(std::move(digits))),
                   "validator disables non-divisible normalized digit sets");
  }
  {
    SkinDigitSpriteSet digits;
    digits.glyphsPerAnimationFrame = 13;
    digits.positive = sprite(13);
    expectDisabled(test_support::validateWithAuthoredBuiltins(
                       numberModel(std::move(digits))),
                   "validator disables Float-only glyph sets on Number");
  }
  {
    SkinDigitSpriteSet digits;
    digits.glyphsPerAnimationFrame = 10;
    digits.positive =
        sprite(LuaSkinTableDecoderPolicy::maxMaterializedSpriteFrames + 1);
    expectDisabled(test_support::validateWithAuthoredBuiltins(
                       numberModel(std::move(digits))),
                   "validator disables digit sets above the frame budget");
  }
}

void validatorAcceptsKindSpecificGlyphBoundaries() {
  SkinDigitSpriteSet numberDigits;
  numberDigits.glyphsPerAnimationFrame = 12;
  numberDigits.positive = sprite(12);
  numberDigits.negative = sprite(12);
  auto number = test_support::validateWithAuthoredBuiltins(
      numberModel(std::move(numberDigits)));
  expect(number.model.has_value() &&
             number.model->disabledOptionalObjects.empty(),
         "validator accepts normalized Number-12 digit sets");

  SkinDigitSpriteSet floatDigits;
  floatDigits.glyphsPerAnimationFrame = 13;
  floatDigits.positive = sprite(13);
  floatDigits.negative = sprite(13);
  auto floating = test_support::validateWithAuthoredBuiltins(
      floatModel(std::move(floatDigits)));
  expect(floating.model.has_value() &&
             floating.model->disabledOptionalObjects.empty(),
         "validator accepts normalized Float-13 digit sets");
}

void validatorRejectsMalformedNumberObjectFormats() {
  const auto reject = [](auto mutate, std::string_view message) {
    auto model = numberModel(validNumberDigits());
    mutate(numberObject(model));
    expectOptionalAndCriticalRejected(std::move(model), message);
  };

  reject([](auto &number) { number.digitCount = -1; },
         "validator rejects negative Number digitCount for optional and "
         "critical objects");
  reject(
      [](auto &number) {
        number.digitCount = NumericGlyphAtlasPolicy::maxNumberDigits + 1;
      },
      "validator rejects Number digitCount above its normalized bound");
  reject(
      [](auto &number) {
        number.zeroPadding = static_cast<SkinZeroPaddingMode>(3);
      },
      "validator rejects invalid Number zero-padding enums");
  reject(
      [](auto &number) {
        number.perDigitOffsets.resize(
            NumericGlyphAtlasPolicy::maxNumberDigitOffsets + 1);
      },
      "validator rejects excess Number per-digit offsets");

  constexpr std::array offsetMembers{&SkinDigitOffset::x, &SkinDigitOffset::y,
                                     &SkinDigitOffset::width,
                                     &SkinDigitOffset::height};
  for (const auto member : offsetMembers) {
    reject(
        [member](auto &number) {
          number.perDigitOffsets.resize(1);
          number.perDigitOffsets.front().*member =
              std::numeric_limits<double>::quiet_NaN();
        },
        "validator rejects every non-finite Number offset component");
  }
}

void validatorRejectsMalformedFloatObjectFormats() {
  const auto reject = [](auto mutate, std::string_view message) {
    auto model = floatModel(validFloatDigits());
    mutate(floatObject(model));
    expectOptionalAndCriticalRejected(std::move(model), message);
  };

  reject([](auto &floating) { floating.integerDigits = -1; },
         "validator rejects negative Float integer digits");
  reject([](auto &floating) { floating.integerDigits = 9; },
         "validator rejects Float integer digits above eight");
  reject([](auto &floating) { floating.fractionalDigits = -1; },
         "validator rejects negative Float fractional digits");
  reject([](auto &floating) { floating.fractionalDigits = 9; },
         "validator rejects Float fractional digits above eight");
  reject(
      [](auto &floating) {
        floating.integerDigits = 5;
        floating.fractionalDigits = 4;
      },
      "validator rejects Float digit sums above eight");
  reject(
      [](auto &floating) {
        floating.zeroPadding = static_cast<SkinZeroPaddingMode>(3);
      },
      "validator rejects invalid Float zero-padding enums");
  reject(
      [](auto &floating) {
        floating.gain = std::numeric_limits<double>::infinity();
      },
      "validator rejects non-finite Float gain");
  reject(
      [](auto &floating) {
        floating.perDigitOffsets.resize(
            NumericGlyphAtlasPolicy::maxFloatDigitOffsets + 1);
      },
      "validator rejects excess Float per-digit offsets");

  constexpr std::array offsetMembers{&SkinDigitOffset::x, &SkinDigitOffset::y,
                                     &SkinDigitOffset::width,
                                     &SkinDigitOffset::height};
  for (const auto member : offsetMembers) {
    reject(
        [member](auto &floating) {
          floating.perDigitOffsets.resize(1);
          floating.perDigitOffsets.front().*member =
              -std::numeric_limits<double>::infinity();
        },
        "validator rejects every non-finite Float offset component");
  }
  reject([](auto &floating) { floating.signVisible = true; },
         "validator rejects visible Float signs without 13-glyph sets");
}

void validatorRetainsGraphRangesThatUpstreamConstructs() {
  const auto result = test_support::validateWithAuthoredBuiltins(graphModel(
      {.value = SkinIntegerPropertyId{1},
       .minimum = std::numeric_limits<int>::min(),
       .maximum = std::numeric_limits<int>::max()}));
  expect(result.model && !result.criticalFailure &&
             result.model->disabledOptionalObjects.empty(),
         "validator retains a Graph range that JsonSkinObjectLoader passes "
         "directly to SkinGraph");
}

void validatorAcceptsDescendingIntegerRateRanges() {
  const auto result = test_support::validateWithAuthoredBuiltins(graphModel(
      {.value = SkinIntegerPropertyId{1}, .minimum = 100, .maximum = 0}));
  expect(result.model && !result.criticalFailure &&
             result.model->disabledOptionalObjects.empty(),
         "validator accepts pinned descending integer Graph ranges");
}

} // namespace

int main() {
  validatorRejectsUnequalInvalidAndExcessDigitSets();
  validatorAcceptsKindSpecificGlyphBoundaries();
  validatorRejectsMalformedNumberObjectFormats();
  validatorRejectsMalformedFloatObjectFormats();
  validatorRetainsGraphRangesThatUpstreamConstructs();
  validatorAcceptsDescendingIntegerRateRanges();
  validatorDiagnosticsNameTheRejectedObject();
  return failures == 0 ? 0 : 1;
}
