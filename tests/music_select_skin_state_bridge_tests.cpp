#include "skin/beatoraja/MusicSelectSkinStateBridge.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

using namespace skin;

int failures = 0;

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testExactPropertyNamespacesAndAbsentValues() {
  MusicSelectSkinFrame frame{.serial = 77, .elapsedMillis = 1'234};
  frame.properties.booleans.emplace(1, true);
  frame.properties.integers.emplace(90, 900);
  frame.properties.imageIndexes.emplace(90, 9);
  frame.properties.rates.emplace(1, 0.25);
  frame.properties.floats.emplace(1, 25.0);
  frame.properties.strings.emplace(1000, "ROOT");
  frame.properties.timers.emplace(2, 555'000);
  frame.properties.namedRates.emplace("musicselect_position", 0.75);

  MusicSelectSkinStateBridge bridge(frame);
  require(bridge.frameSerial() == 77,
          "bridge publishes the immutable frame serial");
  require(bridge.booleanProperty({.value = std::string("select_folderbar")})
              .value,
          "named Boolean selectors resolve through the pinned namespace");
  require(!bridge.booleanProperty(
               {.value = std::string("!select_folderbar")})
               .value,
          "named Boolean negation preserves source recursion");
  require(bridge.integerProperty({.value = 90},
                                 SkinIntegerPropertyDomain::IntegerValue)
                  .value == 900 &&
              bridge.integerProperty({.value = 90},
                                     SkinIntegerPropertyDomain::ImageIndex)
                      .value == 9,
          "Integer Value and Image Index namespaces do not alias");
  require(bridge.floatProperty({.value = 1}, SkinFloatPropertyDomain::Rate)
                  .value == 0.25 &&
              bridge.floatProperty({.value = 1},
                                   SkinFloatPropertyDomain::FloatValue)
                      .value == 25.0 &&
              bridge.floatProperty(
                        {.value = std::string("musicselect_position")},
                        SkinFloatPropertyDomain::Rate)
                      .value == 0.75,
          "Rate, Float Value, and named Rate selectors stay distinct");
  require(bridge.stringProperty({.value = std::string("directory")}).value ==
              "ROOT" &&
              bridge.timerProperty({.value = 2}) == 555'000,
          "named strings and numeric timers resolve from the same frame");

  const auto absentInteger = bridge.integerProperty(
      {.value = 1001}, SkinIntegerPropertyDomain::IntegerValue);
  const auto absentFloat = bridge.floatProperty(
      {.value = 8}, SkinFloatPropertyDomain::Rate);
  const auto absentString = bridge.stringProperty({.value = 1001});
  require(absentInteger.supported &&
              absentInteger.value == std::numeric_limits<std::int32_t>::min(),
          "known absent Integer uses Integer.MIN_VALUE");
  require(absentFloat.supported &&
              absentFloat.value == std::numeric_limits<float>::denorm_min(),
          "known absent Float uses Float.MIN_VALUE");
  require(absentString.supported && absentString.value.empty(),
          "known absent String uses an empty value");
  require(bridge.timerProperty({.value = 3}) ==
              std::numeric_limits<std::int64_t>::min(),
          "known absent Timer uses Long.MIN_VALUE");
}

void testUnknownPropertiesRemainUnsupported() {
  MusicSelectSkinFrame frame;
  MusicSelectSkinStateBridge bridge(frame);
  require(!bridge.booleanProperty({.value = 999'999}).supported &&
              !bridge.integerProperty(
                         {.value = std::string("not_a_beatoraja_property")},
                         SkinIntegerPropertyDomain::IntegerValue)
                   .supported,
          "unknown selectors are not fabricated as built-ins");
  const auto offset = bridge.offsetProperty(199);
  require(offset.supported && offset.value.x == 0.0 &&
              !bridge.offsetProperty(200).supported,
          "only Beatoraja's offset slots exist with default zero values");
}

void testCustomTimerValuesOverrideTheFrameSnapshot() {
  MusicSelectSkinFrame frame;
  frame.properties.timers[10'001] = 42;
  MusicSelectSkinStateBridge bridge(frame);

  require(bridge.timerProperty({.value = 10'001}) == 42,
          "music-select bridge exposes the authored frame timer first");
  bridge.setCustomTimer(10'001, 84);
  require(bridge.timerProperty({.value = 10'001}) == 84,
          "the once-per-frame custom-timer update overrides the snapshot");
}

} // namespace

int main() {
  testExactPropertyNamespacesAndAbsentValues();
  testUnknownPropertiesRemainUnsupported();
  testCustomTimerValuesOverrideTheFrameSnapshot();
  if (failures != 0) {
    std::cerr << failures << " music-select state bridge test(s) failed\n";
    return 1;
  }
  std::cout << "music-select state bridge tests passed\n";
  return 0;
}
