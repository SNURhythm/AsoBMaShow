#include "analysis/JudgedPlaybackResultState.h"
#include "replay/LegacyReplayInputProjection.h"
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

void configureReplayChart(bms_parser::Chart &chart) {
  chart.Meta.KeyMode = 7;
  chart.Meta.TotalNotes = 1;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 1.0;
}

void addNormalNote(bms_parser::Chart &chart, long long noteTimeMicros,
                   int lane) {
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = noteTimeMicros;
  timeline->SetNote(lane, new bms_parser::Note(bms_parser::Parser::NoWav));
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);
}

void addLandmine(bms_parser::Chart &chart, long long noteTimeMicros, int lane,
                 float damage) {
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = noteTimeMicros;
  timeline->SetLandmineNote(lane, new bms_parser::LandmineNote(damage));
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);
}

void addLongNote(bms_parser::Chart &chart, long long headMicros,
                 long long tailMicros, int lane,
                 bms_parser::LongNoteType type) {
  auto *measure = new bms_parser::Measure();
  auto *headTimeline = new bms_parser::TimeLine(8, false);
  auto *tailTimeline = new bms_parser::TimeLine(8, false);
  headTimeline->Timing = headMicros;
  tailTimeline->Timing = tailMicros;
  auto *head = new bms_parser::LongNote(bms_parser::Parser::NoWav, type);
  auto *tail = new bms_parser::LongNote(bms_parser::Parser::NoWav, type);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(lane, head);
  tailTimeline->SetNote(lane, tail);
  measure->TimeLines.push_back(headTimeline);
  measure->TimeLines.push_back(tailTimeline);
  chart.Measures.push_back(measure);
}

void addClassicLongNote(bms_parser::Chart &chart, long long headMicros,
                        long long tailMicros, int lane) {
  addLongNote(chart, headMicros, tailMicros, lane,
              bms_parser::LongNoteType::LongNote);
}

replay::ReplayPlaybackData emptyLegacyPlayback() {
  replay::ReplayPlaybackData playback;
  const RulesetDescriptor ruleset = RulesetDescriptor::Legacy();
  playback.setup.playbackRulesetId = ruleset.id;
  playback.setup.playbackRulesetRevision = ruleset.version;
  playback.setup.keyMode = 7;
  playback.setup.startingGaugePercent = 20.0F;
  playback.legacy.emplace();
  return playback;
}

result_persistence::PersistedChartResult emptyLegacyResult() {
  auto result = persistedResult();
  result.score.finalGauge = 20.0F;
  return result;
}

void synchronizeLegacyInput(replay::ReplayPlaybackData &playback) {
  const auto projection = replay::projectLegacyReplayInput(
      playback.legacy->events, playback.setup.keyMode);
  expect(projection.has_value(), "test legacy input projects");
  if (!projection.has_value()) {
    return;
  }
  playback.input = projection->input;
  playback.legacy->stockScratchDirectionBestEffort =
      projection->stockScratchDirectionBestEffort;
}

replay::ReplayPlaybackData onePerfectLegacyPlayback() {
  auto playback = emptyLegacyPlayback();
  playback.legacy->events = {
      {.action = replay::LegacyPlaybackAction::Press,
       .lane = 0,
       .noteTimeMicros = 1'000,
       .songTimeMicros = 1'000,
       .judgeTimeMicros = 1'000,
       .judgement = PGreat,
       .gauge = 21.0F,
       .gaugeType = GaugeType::Normal,
       .combo = 1,
       .score = 2},
  };
  synchronizeLegacyInput(playback);
  return playback;
}

result_persistence::PersistedChartResult onePerfectLegacyResult() {
  auto result = persistedResult();
  result.score.score = 2;
  result.score.maxCombo = 1;
  result.score.pGreat = 1;
  result.score.finalGauge = 21.0F;
  result.adoptedGaugeHistory = {21.0F};
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
  playback.setup.candidateSelection = gameplay::CandidateSelectionMode::Score;
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
  auto legacyResult = persistedResult();
  legacyResult.score.finalGauge = 100.0F;
  legacyResult.score.clearType = kClearTypeAssistedEasyClearRank;
  legacyResult.adoptedGaugeType = GaugeType::Hard;
  bms_parser::Chart chart;
  configureReplayChart(chart);
  const auto legacy =
      replay::makeLegacyPlaybackAdapter(playback, legacyResult, chart);
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
  auto playback = emptyLegacyPlayback();
  playback.setup.initialLaneCoverPercent = 64;
  playback.setup.laneCoverEnabled = true;
  bms_parser::Chart chart;
  configureReplayChart(chart);

  const auto adapted =
      replay::makeLegacyPlaybackAdapter(playback, emptyLegacyResult(), chart);

  expect(adapted.has_value(), "legacy replay adapts");
  if (adapted.has_value()) {
    expect(adapted->laneCoverEvents.empty(),
           "legacy regression has no timed cover event masking the bug");
    expectInitialLaneCover(*adapted, 64, true, 19, 64,
                           "legacy chart video adapter");
  }
}

void testAdapterPreservesResolvedRuntimeChartPath() {
  auto playback = emptyLegacyPlayback();
  bms_parser::Chart chart;
  configureReplayChart(chart);
  chart.Meta.BmsPath = "/resolved/profile/chart.bms";

  const auto adapted =
      replay::makeLegacyPlaybackAdapter(playback, emptyLegacyResult(), chart);

  expect(adapted.has_value(), "legacy replay with a resolved chart adapts");
  if (adapted.has_value()) {
    expect(adapted->chartMeta.BmsPath == chart.Meta.BmsPath,
           "the adapter preserves the loader-resolved runtime chart path");
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

void testLegacyAdapterRejectsResultMismatch() {
  bms_parser::Chart chart;
  configureReplayChart(chart);
  addNormalNote(chart, 1'000, 0);
  const auto result = onePerfectLegacyResult();
  const auto rejected = [&](replay::ReplayPlaybackData playback,
                            std::string_view fact) {
    const auto adapted =
        replay::makeLegacyPlaybackAdapter(playback, result, chart);
    expect(!adapted.has_value(),
           std::string("legacy annotations cannot replace saved ") +
               std::string(fact));
  };

  auto playback = onePerfectLegacyPlayback();
  expect(replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
         "matching legacy annotations adapt");

  playback = onePerfectLegacyPlayback();
  playback.legacy->events.front().judgement = Great;
  playback.legacy->events.front().score = 1;
  rejected(std::move(playback), "judgement facts");

  playback = onePerfectLegacyPlayback();
  playback.legacy->events.front().score = 1;
  rejected(std::move(playback), "score progression");

  playback = onePerfectLegacyPlayback();
  playback.legacy->events.front().combo = 0;
  rejected(std::move(playback), "combo progression");

  playback = onePerfectLegacyPlayback();
  playback.legacy->events.front().gauge = 22.0F;
  rejected(std::move(playback), "gauge history");
}

void testLegacyAdapterRejectsPresentationMutation() {
  bms_parser::Chart chart;
  configureReplayChart(chart);
  addNormalNote(chart, 1'000, 0);
  const auto result = onePerfectLegacyResult();
  const auto rejected = [&](replay::ReplayPlaybackData playback,
                            std::string_view fact) {
    expect(
        !replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
        std::string("legacy annotations cannot replace saved ") +
            std::string(fact));
  };

  auto playback = onePerfectLegacyPlayback();
  playback.legacy->events.front().lane = 1;
  synchronizeLegacyInput(playback);
  rejected(std::move(playback), "note lane");

  playback = onePerfectLegacyPlayback();
  playback.legacy->events.front().noteTimeMicros = 2'000;
  rejected(std::move(playback), "note timestamp");

  playback = onePerfectLegacyPlayback();
  playback.legacy->events.front().action =
      replay::LegacyPlaybackAction::Release;
  synchronizeLegacyInput(playback);
  rejected(std::move(playback), "note action");

  playback = onePerfectLegacyPlayback();
  playback.legacy->events.front().songTimeMicros = 0;
  synchronizeLegacyInput(playback);
  rejected(std::move(playback), "event schedule");

  playback = onePerfectLegacyPlayback();
  playback.legacy->events.front().judgeTimeMicros = 0;
  rejected(std::move(playback), "judge timestamp");

  playback = onePerfectLegacyPlayback();
  playback.input.front().songTimeMicros = 0;
  rejected(std::move(playback), "raw input schedule");
}

void testLegacyAdapterValidatesGaugeWithoutJudgedEvents() {
  bms_parser::Chart chart;
  configureReplayChart(chart);
  auto playback = emptyLegacyPlayback();
  playback.legacy->events = {
      {.action = replay::LegacyPlaybackAction::Press,
       .lane = 0,
       .noteTimeMicros = -1,
       .songTimeMicros = -1'000,
       .judgeTimeMicros = -1'000,
       .judgement = None,
       .gauge = 20.0F,
       .gaugeType = GaugeType::Normal},
  };
  synchronizeLegacyInput(playback);
  auto result = emptyLegacyResult();
  result.score.finalGauge = 20.0F;

  expect(replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
         "preparation-only annotations retain their initial gauge proof");
  result.score.finalGauge = 21.0F;
  expect(
      !replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
      "an empty adopted history cannot replace the saved final gauge");
}

void testLegacyAdapterRequiresGaugeTickInsideHellCharge() {
  bms_parser::Chart chart;
  configureReplayChart(chart);
  addLongNote(chart, 1'000, 2'000, 0, bms_parser::LongNoteType::HellChargeNote);
  auto playback = emptyLegacyPlayback();
  playback.legacy->events = {
      {.action = replay::LegacyPlaybackAction::Gauge,
       .lane = -1,
       .noteTimeMicros = -1,
       .songTimeMicros = 1'500,
       .judgeTimeMicros = 1'500,
       .judgement = Great,
       .gauge = 20.5F,
       .gaugeType = GaugeType::Normal},
  };
  auto result = emptyLegacyResult();
  result.score.finalGauge = 20.5F;
  result.adoptedGaugeHistory = {20.5F};

  expect(replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
         "a recorded gauge tick inside an HCN interval adapts");
  playback.legacy->events.front().songTimeMicros = 500;
  playback.legacy->events.front().judgeTimeMicros = 500;
  expect(
      !replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
      "an HCN gauge tick cannot be rescheduled before its source note");
}

void testLegacyAdapterAnchorsMineToChartTimestamp() {
  bms_parser::Chart chart;
  configureReplayChart(chart);
  addLandmine(chart, 1'000, 0, 2.0F);
  auto playback = emptyLegacyPlayback();
  playback.legacy->events = {
      {.action = replay::LegacyPlaybackAction::Mine,
       .lane = 0,
       .noteTimeMicros = 1'000,
       .songTimeMicros = 1'000,
       .judgeTimeMicros = 1'000,
       .judgement = None,
       .gauge = 18.0F,
       .gaugeType = GaugeType::Normal},
  };
  auto result = emptyLegacyResult();
  result.score.finalGauge = 18.0F;
  result.adoptedGaugeHistory = {18.0F};

  expect(replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
         "a landmine annotation at its chart timestamp adapts");
  playback.legacy->events.front().songTimeMicros = 500;
  playback.legacy->events.front().judgeTimeMicros = 500;
  expect(
      !replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
      "a landmine cannot be rescheduled away from its chart note");
}

void testLegacyAdapterAcceptsAutomaticEventInputDelayTimestamps() {
  {
    bms_parser::Chart chart;
    configureReplayChart(chart);
    addNormalNote(chart, 1'000, 0);
    auto playback = emptyLegacyPlayback();
    playback.legacy->events = {
        {.action = replay::LegacyPlaybackAction::Miss,
         .lane = 0,
         .noteTimeMicros = 1'000,
         .songTimeMicros = 2'100,
         .judgeTimeMicros = 1'600,
         .judgement = Poor,
         .diffMicros = 600,
         .gauge = 14.0F,
         .gaugeType = GaugeType::Normal,
         .combo = 0,
         .score = 0},
    };
    auto result = persistedResult();
    result.score.comboBreak = 1;
    result.score.poor = 1;
    result.score.slow = 1;
    result.score.finalGauge = 14.0F;
    result.adoptedGaugeHistory = {14.0F};

    expect(
        replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
        "automatic misses preserve their input-delay timestamp offset");
  }

  {
    bms_parser::Chart chart;
    configureReplayChart(chart);
    addLandmine(chart, 1'000, 0, 2.0F);
    auto playback = emptyLegacyPlayback();
    playback.legacy->events = {
        {.action = replay::LegacyPlaybackAction::Mine,
         .lane = 0,
         .noteTimeMicros = 1'000,
         .songTimeMicros = 1'600,
         .judgeTimeMicros = 1'100,
         .judgement = None,
         .gauge = 18.0F,
         .gaugeType = GaugeType::Normal,
         .combo = 0,
         .score = 0},
    };
    auto result = emptyLegacyResult();
    result.score.finalGauge = 18.0F;
    result.adoptedGaugeHistory = {18.0F};

    expect(
        replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
        "landmines preserve their frame and input-delay timestamp offsets");
  }
}

void testLegacyAdapterAcceptsChargeTailMissWithHeadDifference() {
  bms_parser::Chart chart;
  configureReplayChart(chart);
  chart.Meta.TotalNotes = 2;
  chart.Meta.Total = 2.0;
  addLongNote(chart, 1'000, 3'000, 0, bms_parser::LongNoteType::ChargeNote);
  auto playback = emptyLegacyPlayback();
  playback.legacy->events = {
      {.action = replay::LegacyPlaybackAction::Miss,
       .lane = 0,
       .noteTimeMicros = 1'000,
       .songTimeMicros = 2'100,
       .judgeTimeMicros = 1'600,
       .judgement = Poor,
       .diffMicros = 600,
       .gauge = 14.0F,
       .gaugeType = GaugeType::Normal,
       .combo = 0,
       .score = 0},
      {.action = replay::LegacyPlaybackAction::Miss,
       .lane = 0,
       .noteTimeMicros = 3'000,
       .songTimeMicros = 2'100,
       .judgeTimeMicros = 1'600,
       .judgement = Poor,
       .diffMicros = 600,
       .gauge = 8.0F,
       .gaugeType = GaugeType::Normal,
       .combo = 0,
       .score = 0},
  };
  auto result = persistedResult();
  result.score.maxScore = 4;
  result.score.comboBreak = 2;
  result.score.poor = 2;
  result.score.slow = 2;
  result.score.finalGauge = 8.0F;
  result.adoptedGaugeHistory = {14.0F, 8.0F};

  expect(replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
         "a charge tail missed with its head retains the head timing result");
}

void testLegacyAdapterAcceptsAutomaticClassicReleaseSongTimestamp() {
  bms_parser::Chart chart;
  configureReplayChart(chart);
  addClassicLongNote(chart, 1'000, 2'000, 1);
  auto playback = emptyLegacyPlayback();
  playback.legacy->events = {
      {.action = replay::LegacyPlaybackAction::Press,
       .lane = 1,
       .noteTimeMicros = 1'000,
       .songTimeMicros = 1'100,
       .judgeTimeMicros = 1'100,
       .judgement = Good,
       .diffMicros = 100,
       .gauge = 20.0F,
       .gaugeType = GaugeType::Normal,
       .combo = 0,
       .score = 0},
      {.action = replay::LegacyPlaybackAction::Release,
       .lane = 1,
       .noteTimeMicros = 2'000,
       .songTimeMicros = 2'500,
       .judgeTimeMicros = 2'000,
       .judgement = Good,
       .diffMicros = 100,
       .gauge = 20.5F,
       .gaugeType = GaugeType::Normal,
       .combo = 1,
       .score = 0},
  };
  synchronizeLegacyInput(playback);
  auto result = persistedResult();
  result.score.maxCombo = 1;
  result.score.good = 1;
  result.score.slow = 1;
  result.score.finalGauge = 20.5F;
  result.adoptedGaugeHistory = {20.5F};

  expect(replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
         "automatic classic releases preserve the head judgement and input "
         "delay offset");
}

void testLegacyAdapterAcceptsFrameOvershootHellChargeTick() {
  bms_parser::Chart chart;
  configureReplayChart(chart);
  addLongNote(chart, 1'000, 2'000, 0, bms_parser::LongNoteType::HellChargeNote);
  auto playback = emptyLegacyPlayback();
  playback.legacy->events = {
      {.action = replay::LegacyPlaybackAction::Gauge,
       .lane = -1,
       .noteTimeMicros = -1,
       .songTimeMicros = 2'010,
       .judgeTimeMicros = 2'010,
       .judgement = Great,
       .gauge = 20.5F,
       .gaugeType = GaugeType::Normal,
       .combo = 0,
       .score = 0},
  };
  auto result = emptyLegacyResult();
  result.score.finalGauge = 20.5F;
  result.adoptedGaugeHistory = {20.5F};

  expect(replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
         "legacy frame integration may record an HCN tick after its tail");
}

void testLegacyAdapterAcceptsPoorScratchChargeRelease() {
  bms_parser::Chart chart;
  configureReplayChart(chart);
  chart.Meta.TotalNotes = 2;
  chart.Meta.Total = 2.0;
  addLongNote(chart, 1'000, 2'000, 7, bms_parser::LongNoteType::ChargeNote);
  auto playback = emptyLegacyPlayback();
  playback.legacy->events = {
      {.action = replay::LegacyPlaybackAction::Press,
       .lane = 7,
       .noteTimeMicros = 1'000,
       .songTimeMicros = 1'000,
       .judgeTimeMicros = 1'000,
       .judgement = PGreat,
       .gauge = 21.0F,
       .gaugeType = GaugeType::Normal,
       .combo = 1,
       .score = 2},
      {.action = replay::LegacyPlaybackAction::Release,
       .lane = 7,
       .noteTimeMicros = 2'000,
       .songTimeMicros = 2'000,
       .judgeTimeMicros = 2'000,
       .judgement = Poor,
       .gauge = 15.0F,
       .gaugeType = GaugeType::Normal,
       .combo = 0,
       .score = 2},
  };
  synchronizeLegacyInput(playback);
  auto result = persistedResult();
  result.score.maxScore = 4;
  result.score.score = 2;
  result.score.maxCombo = 1;
  result.score.comboBreak = 1;
  result.score.pGreat = 1;
  result.score.poor = 1;
  result.score.finalGauge = 15.0F;
  result.adoptedGaugeHistory = {21.0F, 15.0F};

  expect(replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
         "a non-backspin scratch charge release retains its recorded POOR");
}

void testLegacyAdapterPreservesUnencodedSavedLamp() {
  bms_parser::Chart chart;
  configureReplayChart(chart);
  addNormalNote(chart, 1'000, 0);
  auto result = onePerfectLegacyResult();
  result.score.clearType = kClearTypeNormalClearRank;

  const auto adapted = replay::makeLegacyPlaybackAdapter(
      onePerfectLegacyPlayback(), result, chart);

  expect(adapted.has_value() && adapted->clearType == kClearTypeNormalClearRank,
         "legacy validation preserves a saved lamp absent from annotations");
}

void testLegacyAdapterValidatesCumulativeCourseComboWithCarrySeed() {
  bms_parser::Chart chart;
  configureReplayChart(chart);
  addNormalNote(chart, 1'000, 0);
  auto playback = onePerfectLegacyPlayback();
  playback.legacy->events.front().combo = 2;
  auto result = onePerfectLegacyResult();
  result.score.maxCombo = 2;

  const auto adapted = replay::makeLegacyPlaybackAdapter(
      playback, result, chart, {.carriedCombo = 1, .carriedMaxCombo = 1});

  expect(adapted.has_value(),
         "legacy course stage validates against its cumulative combo seed");
}

void testLegacyAdapterUsesMigrationAdoptedGaugeHistory() {
  bms_parser::Chart chart;
  configureReplayChart(chart);
  chart.Meta.TotalNotes = 2;
  chart.Meta.Total = 2.0;
  addNormalNote(chart, 1'000, 0);
  addNormalNote(chart, 2'000, 1);
  auto playback = emptyLegacyPlayback();
  playback.setup.initialGaugeType = GaugeType::Hazard;
  playback.setup.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
  GaugeStateSnapshot starting{
      .gaugeType = GaugeType::Hazard,
      .selectedGaugeType = GaugeType::ExHard,
      .gaugeAutoShiftLowerBound = GaugeType::Normal,
      .gaugeProfile = GaugeProfile::Standard,
      .gaugeAutoShift = GaugeAutoShiftMode::BestClear,
      .currentGauge = 100.0F,
  };
  starting.gaugeValues[gaugeTypeIndex(GaugeType::AssistedEasy)] = 20.0F;
  starting.gaugeValues[gaugeTypeIndex(GaugeType::Easy)] = 20.0F;
  starting.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] = 20.0F;
  starting.gaugeValues[gaugeTypeIndex(GaugeType::Hard)] = 1.0F;
  starting.gaugeValues[gaugeTypeIndex(GaugeType::ExHard)] = 1.0F;
  starting.gaugeValues[gaugeTypeIndex(GaugeType::Hazard)] = 100.0F;
  playback.setup.startingGaugeState = starting;
  playback.legacy->events = {
      {.action = replay::LegacyPlaybackAction::Press,
       .lane = 0,
       .noteTimeMicros = 1'000,
       .songTimeMicros = 1'000,
       .judgeTimeMicros = 1'000,
       .judgement = PGreat,
       .gauge = 100.0F,
       .gaugeType = GaugeType::Hazard,
       .combo = 1,
       .score = 2},
      {.action = replay::LegacyPlaybackAction::Press,
       .lane = 1,
       .noteTimeMicros = 2'000,
       .songTimeMicros = 2'000,
       .judgeTimeMicros = 2'000,
       .judgement = Bad,
       .gauge = 18.0F,
       .gaugeType = GaugeType::Normal,
       .combo = 0,
       .score = 2},
  };
  synchronizeLegacyInput(playback);
  auto result = persistedResult();
  result.score.score = 2;
  result.score.maxScore = 4;
  result.score.maxCombo = 1;
  result.score.comboBreak = 1;
  result.score.pGreat = 1;
  result.score.bad = 1;
  result.score.finalGauge = 18.0F;
  result.score.clearType = kClearTypeNormalClearRank;
  result.adoptedGaugeType = GaugeType::Normal;
  result.adoptedGaugeHistory = {18.0F};

  expect(replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
         "legacy validation uses the migration's adopted-gauge projection");

  playback.legacy->events.front().gauge = 0.0F;
  expect(
      !replay::makeLegacyPlaybackAdapter(playback, result, chart).has_value(),
      "an omitted earlier gauge type cannot forge a survival failure");
}

void testLegacyAdapterAcceptsClassicBadHeadPreJudgeSnapshot() {
  bms_parser::Chart chart;
  configureReplayChart(chart);
  addClassicLongNote(chart, 1'000, 2'000, 1);
  auto playback = emptyLegacyPlayback();
  playback.setup.initialGaugeType = GaugeType::Hard;
  playback.setup.startingGaugeState = GaugeStateSnapshot{
      .gaugeType = GaugeType::Hard,
      .selectedGaugeType = GaugeType::Hard,
      .gaugeProfile = GaugeProfile::Standard,
      .gaugeAutoShift = GaugeAutoShiftMode::None,
      .currentGauge = 2.0F,
  };
  playback.setup.startingGaugeState
      ->gaugeValues[gaugeTypeIndex(GaugeType::Hard)] = 2.0F;
  playback.legacy->events = {
      {.action = replay::LegacyPlaybackAction::Press,
       .lane = 1,
       .noteTimeMicros = 1'000,
       .songTimeMicros = 1'100,
       .judgeTimeMicros = 1'100,
       .judgement = Bad,
       .diffMicros = 100,
       .gauge = 2.0F,
       .gaugeType = GaugeType::Hard,
       .combo = 3,
       .score = 0},
  };
  synchronizeLegacyInput(playback);
  auto result = persistedResult();
  result.score.maxCombo = 3;
  result.score.comboBreak = 1;
  result.score.bad = 1;
  result.score.slow = 1;
  result.score.finalGauge = 2.0F;
  result.adoptedGaugeType = GaugeType::Hard;
  result.adoptedGaugeHistory = {2.0F};
  result.judgementTiming.emplace();
  result.judgementTiming->byJudgement[Bad].slow = 1;

  const auto adapted = replay::makeLegacyPlaybackAdapter(
      playback, result, chart, {.carriedCombo = 3, .carriedMaxCombo = 3});

  expect(adapted.has_value(),
         "classic Bad head fallback accepts its historical pre-judge combo");
  if (adapted.has_value()) {
    const RhythmState state = analysis::BuildResultState(
        chart, *adapted, GaugeProfile::Standard, nullptr, 3, 3);
    expect(!state.activeGaugeFailed(),
           "classic Bad head fallback does not invent a gauge failure");
  }
}

void testLegacyAdapterValidatesDeferredClassicHeadSnapshot() {
  bms_parser::Chart chart;
  configureReplayChart(chart);
  addClassicLongNote(chart, 1'000, 2'000, 1);
  auto playback = emptyLegacyPlayback();
  playback.legacy->events = {
      {.action = replay::LegacyPlaybackAction::Press,
       .lane = 1,
       .noteTimeMicros = 1'000,
       .songTimeMicros = 1'000,
       .judgeTimeMicros = 1'000,
       .judgement = Good,
       .gauge = 20.0F,
       .gaugeType = GaugeType::Normal,
       .combo = 1,
       .score = 0},
      {.action = replay::LegacyPlaybackAction::Release,
       .lane = 1,
       .noteTimeMicros = 2'000,
       .songTimeMicros = 2'000,
       .judgeTimeMicros = 2'000,
       .judgement = Good,
       .gauge = 20.5F,
       .gaugeType = GaugeType::Normal,
       .combo = 2,
       .score = 0},
  };
  synchronizeLegacyInput(playback);
  auto result = persistedResult();
  result.score.maxCombo = 2;
  result.score.good = 1;
  result.score.finalGauge = 20.5F;
  result.adoptedGaugeHistory = {20.5F};
  const replay::ReplayMaterializationSeed seed{.carriedCombo = 1,
                                               .carriedMaxCombo = 1};

  expect(replay::makeLegacyPlaybackAdapter(playback, result, chart, seed)
             .has_value(),
         "deferred classic head accepts its historical counter snapshot");
  playback.legacy->events.front().combo = 99;
  expect(!replay::makeLegacyPlaybackAdapter(playback, result, chart, seed)
              .has_value(),
         "deferred classic head cannot inject a false HUD combo");
  playback.legacy->events.front().combo = 1;
  playback.legacy->events.front().gauge = 0.0F;
  playback.legacy->events.front().gaugeType = GaugeType::Hard;
  expect(!replay::makeLegacyPlaybackAdapter(playback, result, chart, seed)
              .has_value(),
         "deferred classic head cannot inject a false gauge failure");
}

void testJudgedPlaybackWithoutRecordedLaneCoverKeepsSettingsFallback() {
  expect(replay::initialLaneCoverPercentForRendering(JudgedPlaybackData{}.setup,
                                                     19) == 19,
         "judged playback without a raw setup keeps the settings fallback");
}

} // namespace

int main() {
  testAdaptersRetainCompletePlaybackSetup();
  testLegacyAdapterCarriesEnabledInitialLaneCoverForChartVideo();
  testAdapterPreservesResolvedRuntimeChartPath();
  testMaterializedAdapterCarriesDisabledRememberedLaneCoverForCourseVideo();
  testMaterializedAdapterRejectsResultMismatch();
  testLegacyAdapterRejectsResultMismatch();
  testLegacyAdapterRejectsPresentationMutation();
  testLegacyAdapterValidatesGaugeWithoutJudgedEvents();
  testLegacyAdapterRequiresGaugeTickInsideHellCharge();
  testLegacyAdapterAnchorsMineToChartTimestamp();
  testLegacyAdapterAcceptsAutomaticEventInputDelayTimestamps();
  testLegacyAdapterAcceptsChargeTailMissWithHeadDifference();
  testLegacyAdapterAcceptsAutomaticClassicReleaseSongTimestamp();
  testLegacyAdapterAcceptsFrameOvershootHellChargeTick();
  testLegacyAdapterAcceptsPoorScratchChargeRelease();
  testLegacyAdapterPreservesUnencodedSavedLamp();
  testLegacyAdapterValidatesCumulativeCourseComboWithCarrySeed();
  testLegacyAdapterUsesMigrationAdoptedGaugeHistory();
  testLegacyAdapterAcceptsClassicBadHeadPreJudgeSnapshot();
  testLegacyAdapterValidatesDeferredClassicHeadSnapshot();
  testJudgedPlaybackWithoutRecordedLaneCoverKeepsSettingsFallback();
  if (failures != 0) {
    std::cerr << failures << " replay playback adapter test(s) failed\n";
    return 1;
  }
  std::cout << "Replay playback adapter tests passed\n";
  return 0;
}
