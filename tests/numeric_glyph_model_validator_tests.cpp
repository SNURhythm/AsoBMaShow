#include "skin/beatoraja/LuaSkinTableDecoder.h"
#include "skin/beatoraja/SkinModelValidator.h"

#include <cstddef>
#include <iostream>
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

void validatorRejectsUnequalInvalidAndExcessDigitSets() {
  {
    SkinDigitSpriteSet digits;
    digits.glyphsPerAnimationFrame = 12;
    digits.positive = sprite(24);
    digits.negative = sprite(12);
    expectDisabled(SkinModelValidator{}.validate(numberModel(std::move(digits))),
                   "validator disables unequal signed animation-frame sets");
  }
  {
    SkinDigitSpriteSet digits;
    digits.glyphsPerAnimationFrame = 10;
    digits.positive = sprite(11);
    expectDisabled(SkinModelValidator{}.validate(numberModel(std::move(digits))),
                   "validator disables non-divisible normalized digit sets");
  }
  {
    SkinDigitSpriteSet digits;
    digits.glyphsPerAnimationFrame = 13;
    digits.positive = sprite(13);
    expectDisabled(SkinModelValidator{}.validate(numberModel(std::move(digits))),
                   "validator disables Float-only glyph sets on Number");
  }
  {
    SkinDigitSpriteSet digits;
    digits.glyphsPerAnimationFrame = 10;
    digits.positive =
        sprite(LuaSkinTableDecoderPolicy::maxMaterializedSpriteFrames + 1);
    expectDisabled(SkinModelValidator{}.validate(numberModel(std::move(digits))),
                   "validator disables digit sets above the frame budget");
  }
}

void validatorAcceptsKindSpecificGlyphBoundaries() {
  SkinDigitSpriteSet numberDigits;
  numberDigits.glyphsPerAnimationFrame = 12;
  numberDigits.positive = sprite(12);
  numberDigits.negative = sprite(12);
  auto number =
      SkinModelValidator{}.validate(numberModel(std::move(numberDigits)));
  expect(number.model.has_value() &&
             number.model->disabledOptionalObjects.empty(),
         "validator accepts normalized Number-12 digit sets");

  SkinDigitSpriteSet floatDigits;
  floatDigits.glyphsPerAnimationFrame = 13;
  floatDigits.positive = sprite(13);
  floatDigits.negative = sprite(13);
  auto floating =
      SkinModelValidator{}.validate(floatModel(std::move(floatDigits)));
  expect(floating.model.has_value() &&
             floating.model->disabledOptionalObjects.empty(),
         "validator accepts normalized Float-13 digit sets");
}

} // namespace

int main() {
  validatorRejectsUnequalInvalidAndExcessDigitSets();
  validatorAcceptsKindSpecificGlyphBoundaries();
  return failures == 0 ? 0 : 1;
}
