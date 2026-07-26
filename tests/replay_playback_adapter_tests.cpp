#include "replay/LegacyReplayPlaybackAdapter.h"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void expectInitialLaneCover(const JudgedPlaybackData &playback,
                            int rememberedPercent, bool enabled,
                            int settingsFallbackPercent, int renderedPercent,
                            std::string_view context) {
  expect(playback.initialLaneCoverPercent == rememberedPercent,
         std::string(context) + " preserves the recorded percentage");
  expect(playback.laneCoverEnabled == enabled,
         std::string(context) + " preserves the recorded enabled flag");
  expect(initialLaneCoverPercentForRendering(
             playback, settingsFallbackPercent) == renderedPercent,
         std::string(context) +
             " initializes video rendering from the recorded state");
}

result_persistence::PersistedChartResult persistedResult() {
  result_persistence::PersistedChartResult result;
  result.score.chartPath = "chart.bms";
  result.score.chartMd5 = "md5";
  result.score.chartSha256 = "sha256";
  result.score.chartTitle = "Title";
  result.score.chartArtist = "Artist";
  result.score.maxScore = 2;
  result.keyMode = 7;
  return result;
}

void testLegacyAdapterCarriesEnabledInitialLaneCoverForChartVideo() {
  replay::ReplayPlaybackData playback;
  playback.setup.initialLaneCoverPercent = 64;
  playback.setup.laneCoverEnabled = true;
  playback.legacy.emplace();

  const auto adapted = replay::makeLegacyPlaybackAdapter(
      playback, persistedResult(), bms_parser::ChartMeta{});

  expect(adapted.has_value(), "legacy replay adapts");
  if (adapted.has_value()) {
    expect(adapted->laneCoverEvents.empty(),
           "legacy regression has no timed cover event masking the bug");
    expectInitialLaneCover(*adapted, 64, true, 19, 64,
                           "legacy chart video adapter");
  }
}

void testMaterializedAdapterCarriesDisabledRememberedLaneCoverForCourseVideo() {
  replay::ReplayPlaybackData playback;
  playback.setup.initialLaneCoverPercent = 73;
  playback.setup.laneCoverEnabled = false;
  replay::MaterializedReplay materialized;
  gameplay::GameplayRulesetPolicy policy;

  const JudgedPlaybackData adapted = replay::makeMaterializedPlaybackAdapter(
      playback, materialized, policy, persistedResult(),
      bms_parser::ChartMeta{});

  expect(adapted.laneCoverEvents.empty(),
         "materialized regression has no timed cover event masking the bug");
  expectInitialLaneCover(adapted, 73, false, 19, 0,
                         "materialized course video adapter");
}

void testJudgedPlaybackWithoutRecordedLaneCoverKeepsSettingsFallback() {
  expect(initialLaneCoverPercentForRendering(JudgedPlaybackData{}, 19) == 19,
         "judged playback without a raw setup keeps the settings fallback");
}

} // namespace

int main() {
  testLegacyAdapterCarriesEnabledInitialLaneCoverForChartVideo();
  testMaterializedAdapterCarriesDisabledRememberedLaneCoverForCourseVideo();
  testJudgedPlaybackWithoutRecordedLaneCoverKeepsSettingsFallback();
  if (failures != 0) {
    std::cerr << failures << " replay playback adapter test(s) failed\n";
    return 1;
  }
  std::cout << "Replay playback adapter tests passed\n";
  return 0;
}
