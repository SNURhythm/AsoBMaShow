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
  expect(playback.setup.initialLaneCoverPercent == rememberedPercent,
         std::string(context) + " preserves the recorded percentage");
  expect(playback.setup.laneCoverEnabled == enabled,
         std::string(context) + " preserves the recorded enabled flag");
  expect(replay::initialLaneCoverPercentForRendering(
             playback.setup, settingsFallbackPercent) == renderedPercent,
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

replay::ReplayPlaybackData replayWithNonDefaultSetup() {
  replay::ReplayPlaybackData playback;
  playback.setup.chartMd5 = std::string(32, 'a');
  playback.setup.chartSha256 = std::string(64, 'b');
  playback.setup.keyMode = 14;
  playback.setup.longNoteMode = 2;
  playback.setup.hasUndefinedLongNotes = true;
  playback.setup.randomSeed = 17U;
  playback.setup.randomPrng = "std::mt19937_64";
  playback.setup.randomValues = {3, 1, 4};
  playback.setup.playOption = "R-RANDOM";
  playback.setup.playOptionSeed = 23;
  playback.setup.playOption2 = "MIRROR";
  playback.setup.playOption2Seed = 29;
  playback.setup.doublePlayOption = replay::DoublePlayOption::Flip;
  playback.setup.assistOption = assist_options::kDrag;
  playback.setup.initialGaugeType = GaugeType::ExHard;
  playback.setup.gaugeProfile = GaugeProfile::Standard;
  playback.setup.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  playback.setup.gaugeAutoShiftLowerBound = GaugeType::Easy;
  playback.setup.playbackRulesetId = "beatoraja";
  playback.setup.playbackRulesetRevision = 2;
  playback.setup.playbackRatePercent = 75;
  playback.setup.playbackMode = audio::PlaybackMode::TimeStretch;
  playback.setup.candidateSelection =
      gameplay::CandidateSelectionMode::Score;
  playback.setup.judgeWindowScalePercent = 90;
  playback.setup.startingGaugePercent = 37.0F;
  playback.setup.startingGaugeState = GaugeStateSnapshot{
      .gaugeType = GaugeType::Hard,
      .selectedGaugeType = GaugeType::ExHard,
      .gaugeAutoShiftLowerBound = GaugeType::Easy,
      .gaugeProfile = GaugeProfile::Standard,
      .gaugeAutoShift = GaugeAutoShiftMode::BestClear,
      .currentGauge = 100.0F,
  };
  playback.setup.clubMode = true;
  playback.setup.initialLaneCoverPercent = 64;
  playback.setup.laneCoverEnabled = true;
  return playback;
}

void testAdaptersRetainCompletePlaybackSetup() {
  auto playback = replayWithNonDefaultSetup();
  playback.legacy.emplace();
  const auto legacy = replay::makeLegacyPlaybackAdapter(
      playback, persistedResult(), bms_parser::ChartMeta{});
  expect(legacy.has_value(), "nondefault legacy replay adapts");
  if (legacy.has_value()) {
    expect(legacy->setup == playback.setup,
           "legacy adapter retains the complete raw playback setup");
  }

  replay::MaterializedReplay materialized;
  gameplay::GameplayRulesetPolicy policy;
  const auto judged = replay::makeMaterializedPlaybackAdapter(
      playback, materialized, policy, persistedResult(),
      bms_parser::ChartMeta{});
  expect(judged.has_value(), "matching materialized replay adapts");
  if (judged.has_value()) {
    expect(judged->setup == playback.setup,
           "materialized adapter retains the complete raw playback setup");
  }
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

  const auto adapted = replay::makeMaterializedPlaybackAdapter(
      playback, materialized, policy, persistedResult(),
      bms_parser::ChartMeta{});

  expect(adapted.has_value(), "matching materialized course replay adapts");
  if (adapted.has_value()) {
    expect(adapted->laneCoverEvents.empty(),
           "materialized regression has no timed cover event masking the bug");
    expectInitialLaneCover(*adapted, 73, false, 19, 0,
                           "materialized course video adapter");
  }
}

void testMaterializedAdapterRejectsResultMismatch() {
  replay::ReplayPlaybackData playback;
  gameplay::GameplayRulesetPolicy policy;
  const auto rejected = [&](replay::MaterializedReplay materialized,
                            std::string_view fact) {
    const auto adapted = replay::makeMaterializedPlaybackAdapter(
        playback, materialized, policy, persistedResult(),
        bms_parser::ChartMeta{});
    expect(!adapted.has_value(),
           std::string("materialized input cannot replace saved ") +
               std::string(fact));
  };

  replay::MaterializedReplay materialized;
  materialized.attempt.score = 1;
  rejected(materialized, "score facts");

  materialized = {};
  materialized.attempt.maxCombo = 1;
  rejected(materialized, "combo facts");

  materialized = {};
  materialized.attempt.judgeCounts[PGreat] = 1;
  rejected(materialized, "judgement facts");

  materialized = {};
  materialized.attempt.gauge = 1.0F;
  rejected(materialized, "gauge facts");

  materialized = {};
  materialized.attempt.clearTypeRank = kClearTypeNormalClearRank;
  rejected(materialized, "clear facts");

  materialized = {};
  materialized.gaugeHistory = {1.0F};
  rejected(materialized, "gauge-history facts");
}

void testJudgedPlaybackWithoutRecordedLaneCoverKeepsSettingsFallback() {
  expect(replay::initialLaneCoverPercentForRendering(
             JudgedPlaybackData{}.setup, 19) == 19,
         "judged playback without a raw setup keeps the settings fallback");
}

} // namespace

int main() {
  testAdaptersRetainCompletePlaybackSetup();
  testLegacyAdapterCarriesEnabledInitialLaneCoverForChartVideo();
  testMaterializedAdapterCarriesDisabledRememberedLaneCoverForCourseVideo();
  testMaterializedAdapterRejectsResultMismatch();
  testJudgedPlaybackWithoutRecordedLaneCoverKeepsSettingsFallback();
  if (failures != 0) {
    std::cerr << failures << " replay playback adapter test(s) failed\n";
    return 1;
  }
  std::cout << "Replay playback adapter tests passed\n";
  return 0;
}
