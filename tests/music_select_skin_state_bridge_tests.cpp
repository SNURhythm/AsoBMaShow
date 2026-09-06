#include "skin/beatoraja/MusicSelectSkinStateBridge.h"

#include "music_select_runtime_ledger_assertions.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
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
  require(!absentInteger.supported,
          "an unimplemented Integer factory slot remains absent");
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

void testPublishedSongResourcesOverrideChartPathFlags() {
  MusicSelectSkinFrame frame;
  frame.properties.booleans[190] = false;
  frame.properties.booleans[191] = true;
  frame.properties.booleans[192] = false;
  frame.properties.booleans[193] = true;
  frame.properties.booleans[194] = false;
  frame.properties.booleans[195] = true;
  MusicSelectSkinStateBridge bridge(frame);
  bridge.setPublishedSongResources(
      {.stageFile = false, .banner = true, .backBmp = false});

  require(bridge.booleanProperty({190}).value &&
              !bridge.booleanProperty({191}).value &&
              !bridge.booleanProperty({192}).value &&
              bridge.booleanProperty({193}).value &&
              bridge.booleanProperty({194}).value &&
              !bridge.booleanProperty({195}).value,
          "selector artwork flags read published SongBar pixmaps and never "
          "a chart path or selector backbmp declaration");
}

void testSelectedChartInformationPublishesSelectorGraphs() {
  MusicSelectSkinFrame frame;
  auto chart = std::make_shared<SkinGameplayChartGraphState>();
  chart->mainBpm = 174.0;
  chart->minimumBpm = 87.0;
  chart->maximumBpm = 348.0;
  chart->normalDistribution = {{{1, 2, 3, 4, 5, 6, 7}}};
  chart->bpmSeries = {{.chartTimeMicros = 1'000'000,
                       .bpm = 174.0,
                       .scroll = 1.0,
                       .bpmTimesScroll = 174.0,
                       .graphSpeed = 174.0,
                       .emitsGraphPoint = true}};
  frame.gameplayGraph.chart = std::move(chart);
  MusicSelectSkinStateBridge bridge(frame);
  const auto graph = bridge.gameplayGraphState();
  require(graph.normalDistribution.size() == 1 &&
              graph.normalDistribution.front()[6] == 7 &&
              graph.bpmSeries.size() == 1 && graph.mainBpm == 174.0 &&
              graph.minimumBpm == 87.0 && graph.maximumBpm == 348.0,
          "selector state exposes delayed SongInformation distribution and "
          "BPM graph data to generic Beatoraja graph objects");
}

void testSkinTimerWritesUseBeatorajaCustomTimerRules() {
  MusicSelectSkinFrame frame;
  MusicSelectSkinStateBridge local(frame);

  require(!local.setTimerProperty(9'999, 1) &&
              !local.setTimerProperty(20'000, 1),
          "skin timer writes reject IDs outside Beatoraja's custom range");
  require(local.setTimerProperty(10'000, 123) &&
              local.timerProperty({.value = 10'000}) == 123,
          "passive custom timers are created by skin writes");

  std::map<int, std::int64_t> persistent{{10'001, 50}};
  const std::set<int> active{10'001};
  MusicSelectSkinStateBridge first(frame, persistent, active);
  require(first.setTimerProperty(10'001, 75) && persistent.at(10'001) == 50,
          "writes to callback-backed custom timers are accepted and ignored");
  require(first.setTimerProperty(10'002, 84) && persistent.at(10'002) == 84,
          "writes create persistent passive custom timers");

  MusicSelectSkinStateBridge next(frame, persistent, active);
  require(next.timerProperty({.value = 10'002}) == 84,
          "passive custom timer values survive music-select frames");
}

} // namespace

int main(int argc, char **argv) {
  testExactPropertyNamespacesAndAbsentValues();
  testUnknownPropertiesRemainUnsupported();
  testCustomTimerValuesOverrideTheFrameSnapshot();
  testPublishedSongResourcesOverrideChartPathFlags();
  testSelectedChartInformationPublishesSelectorGraphs();
  testSkinTimerWritesUseBeatorajaCustomTimerRules();
  return music_select_runtime_ledger_assertions::finish(
      argc, argv, "music_select_skin_state_bridge_tests", failures,
      "music-select state bridge test(s) failed",
      "music-select state bridge tests passed");
}
