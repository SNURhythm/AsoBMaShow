#include "replay/ReplayPlaybackDriver.h"
#include "replay/ReplayPlaybackMaterializer.h"

#include "ScoreProvenance.h"
#include "bms_parser.hpp"

#include <iostream>
#include <tuple>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace replay;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

ReplayChartDocument document() {
  ReplayChartDocument value;
  value.timeBounds = {.completionSongTimeMicros = 1'000};
  value.playback.setup.chart = {.md5 = repeated('b', 32),
                                .sha256 = repeated('a', 64),
                                .keyMode = 7};
  value.playback.setup.longNoteMode = 1;
  value.playback.input = {
      {.songTimeMicros = 100,
       .control = {.kind = LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true},
      {.songTimeMicros = 200,
       .control = {.kind = LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = false},
  };
  value.playback.touchSamples = {
      {.action = ReplayTouchAction::Down,
       .fingerId = 4,
       .songTimeMicros = 150,
       .x = 0.25F,
       .y = 0.75F},
  };
  value.playback.laneCoverEvents = {
      {.songTimeMicros = 175,
       .noteStartPositionPercent = 37,
       .resetVisibleTimeReference = true},
  };
  return value;
}

result_persistence::ModernChartResult savedResult() {
  result_persistence::ModernChartResult value;
  value.resultId = 17;
  value.attemptId = "123e4567-e89b-42d3-a456-426614174000";
  value.score.chartPath = "library/chart.bms";
  value.score.chartMd5 = repeated('b', 32);
  value.score.chartSha256 = repeated('a', 64);
  value.score.chartTitle = "Title";
  value.score.chartArtist = "Artist";
  value.score.longNoteMode = 1;
  value.score.score = 7;
  value.score.maxScore = 10;
  value.score.maxCombo = 4;
  value.score.comboBreak = 1;
  value.score.pGreat = 3;
  value.score.great = 1;
  value.score.good = 1;
  value.score.finalGauge = 82.5F;
  value.score.clearType = kClearTypeNormalClearRank;
  value.score.provenance = ScoreProvenance::Legacy();
  value.keyMode = 7;
  value.adoptedGaugeType = GaugeType::Normal;
  value.adoptedGaugeHistory = {20.0F, 48.5F, 82.5F};
  value.playedAtUnixMillis = 1'700'000'000'123LL;
  value.resultFingerprint = result_persistence::modernResultFingerprint(value);
  return value;
}

bms_parser::Chart oneNoteChart() {
  bms_parser::Chart chart;
  chart.Meta.BmsPath = "library/chart.bms";
  chart.Meta.MD5 = repeated('b', 32);
  chart.Meta.SHA256 = repeated('a', 64);
  chart.Meta.KeyMode = 7;
  chart.Meta.Rank = 2;
  chart.Meta.TotalNotes = 1;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 200.0;
  chart.Meta.LnMode = 1;
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = 500'000;
  timeline->SetNote(0, new bms_parser::Note(1));
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);
  return chart;
}

void testConcreteMaterializerBuildsConsumerTrackOnlyAfterAgreement() {
  auto chart = oneNoteChart();
  auto replay = document();
  replay.timeBounds = {.completionSongTimeMicros = 2'000'000};
  replay.playback.input = {
      {.songTimeMicros = 500'000,
       .control = {.kind = LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true},
      {.songTimeMicros = 510'000,
       .control = {.kind = LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = false},
  };

  auto saved = savedResult();
  ScoreProvenanceBuildInput provenance;
  provenance.chartMeta = chart.Meta;
  provenance.longNoteMode = 1;
  provenance.sourceJudgeRank = chart.Meta.Rank;
  provenance.effectiveJudgeWindows = {
      {PGreat, {-20'000, 20'000}}, {Great, {-50'000, 50'000}},
      {Good, {-100'000, 100'000}}, {Bad, {-200'000, 200'000}},
      {Kpoor, {-1'000'000, 0}},
  };
  provenance.totalNotes = 1;
  provenance.authoredGaugeTotal = 200.0;
  provenance.effectiveGaugeTotal = 200.0;
  provenance.inputDevices = {InputDeviceCategory::Keyboard};
  saved.score.provenance = makeScoreProvenance(provenance);
  saved.score.maxScore = 2;
  saved.score.chartPath = "library/chart.bms";
  saved.keyMode = 7;
  replay.playback.setup.ruleset = saved.score.provenance.ruleset;
  replay.playback.setup.candidateSelection =
      saved.score.provenance.stages.front().candidateSelection;
  replay.playback.setup.gaugeProfile = saved.score.provenance.gaugeProfile;
  replay.playback.setup.initialGaugeType = saved.score.provenance.gaugeType;
  replay.playback.setup.longNoteMode = 1;
  replay.playback.setup.playback = saved.score.provenance.playback;

  const auto first = ReplayPlaybackMaterializer::materializeForConsumers(
      replay, ReplaySetupSource::LocalCapture, saved, chart);
  expect(first.state == ReplayPlaybackMaterializationState::ResultMismatch &&
             first.judgedResult.has_value() && !first.replayData.has_value(),
         "consumer track stays unavailable when replay judging disagrees");
  if (!first.judgedResult.has_value()) {
    return;
  }

  saved = *first.judgedResult;
  const auto matched = ReplayPlaybackMaterializer::materializeForConsumers(
      replay, ReplaySetupSource::LocalCapture, saved, chart);
  expect(matched.matched() && matched.replayData.has_value() &&
             !matched.replayData->events.empty() &&
             matched.replayData->finalScore == saved.score.score &&
             matched.replayData->provenance == saved.score.provenance,
         "verified replay yields one in-memory judged track for consumers");
  expect(matched.replayData && matched.replayData->touchSamples.size() == 1 &&
             matched.replayData->laneCoverEvents.size() == 1,
         "consumer track preserves BRD-owned touch and lane-cover streams");
}

void testDriverMergesStreamsWithoutChangingTheirTiming() {
  const auto replay = document();
  ReplayPlaybackDriver driver(replay, ReplaySetupSource::LocalCapture);
  std::vector<std::string> delivered;
  ReplayPlaybackSink sink{
      .input = [&](const InputTransition &event, std::string &) {
        delivered.push_back("input:" + std::to_string(event.songTimeMicros));
        return true;
      },
      .touch = [&](const ReplayTouchSample &event, std::string &) {
        delivered.push_back("touch:" + std::to_string(event.songTimeMicros));
        return true;
      },
      .laneCover = [&](const ReplayLaneCoverEvent &event, std::string &) {
        delivered.push_back("cover:" + std::to_string(event.songTimeMicros));
        return true;
      }};

  auto advanced = driver.advanceTo(160, sink);
  expect(advanced.state == ReplayPlaybackDriverState::Advanced &&
             delivered ==
                 std::vector<std::string>{"input:100", "touch:150"},
         "driver emits raw logical input and touch at recorded times");
  advanced = driver.advanceTo(200, sink);
  expect(advanced.state == ReplayPlaybackDriverState::Advanced &&
             delivered == std::vector<std::string>{
                              "input:100", "touch:150", "cover:175",
                              "input:200"},
         "driver globally merges lane-cover and input without retiming");
  advanced = driver.advanceTo(1'000, sink);
  expect(advanced.state == ReplayPlaybackDriverState::Complete &&
             driver.complete(),
         "driver completes only at the parsed completion boundary");
}

void testDriverRejectsReverseTimeAndBoundsEachAdvance() {
  const auto replay = document();
  ReplayPlaybackDriver driver(replay, ReplaySetupSource::LocalCapture);
  ReplayPlaybackSink sink;
  expect(driver.advanceTo(200, sink).state ==
             ReplayPlaybackDriverState::Advanced,
         "first monotonic advance succeeds with optional auxiliary sinks");
  expect(driver.advanceTo(199, sink).state ==
             ReplayPlaybackDriverState::NonMonotonicAdvance,
         "reverse playback time fails closed");

  ReplayPlaybackDriver bounded(replay, ReplaySetupSource::LocalCapture);
  const auto exhausted = bounded.advanceTo(1'000, sink, 2);
  expect(exhausted.state == ReplayPlaybackDriverState::WorkLimitExceeded &&
             !bounded.complete(),
         "per-run event budget bounds adversarial playback work");
}

void testLogicalGameplayAdapterOwnsLaneAndScratchMapping() {
  for (const auto &layout : kReplayKeyModeLayouts) {
    for (int player = 1; player <= layout.players; ++player) {
      for (int lane = 0; lane < layout.logicalLanesPerPlayer; ++lane) {
        const LogicalControl logical{.kind = LogicalControlKind::Lane,
                                     .player = player,
                                     .lane = lane};
        const auto physical = physicalChartLaneForLogicalControl(
            layout.keyMode, logical);
        expect(physical &&
                   logicalControlForChartLane(layout.keyMode, *physical,
                                              false) == logical,
               "logical lane mapping round-trips through one authority");
      }
      if (layout.hasDirectionalScratch) {
        const LogicalControl scratch{
            .kind = LogicalControlKind::ScratchCounterClockwise,
            .player = player,
            .lane = -1};
        const auto physical = physicalChartLaneForLogicalControl(
            layout.keyMode, scratch);
        expect(physical &&
                   logicalControlForChartLane(
                       layout.keyMode, *physical, true,
                       LogicalControlKind::ScratchCounterClockwise) == scratch,
               "directional scratch mapping round-trips through one authority");
      }
    }
  }

  struct Edge {
    bool pressed = false;
    int lane = -1;
    bool backSpin = false;
    double delay = 0.0;
  };
  std::vector<Edge> edges;
  ReplayLogicalGameplayAdapter adapter(
      14,
      {.pressLane = [&](int lane, double delay) {
         edges.push_back({.pressed = true, .lane = lane, .delay = delay});
       },
       .releaseLane = [&](int lane, double delay, bool backSpin) {
         edges.push_back({.pressed = false,
                          .lane = lane,
                          .backSpin = backSpin,
                          .delay = delay});
       }});
  const std::vector<InputTransition> batch{
      {.songTimeMicros = 100,
       .control = {.kind = LogicalControlKind::Lane,
                   .player = 2,
                   .lane = 2},
       .pressed = true},
      {.songTimeMicros = 100,
       .control = {.kind = LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
      {.songTimeMicros = 200,
       .control = {.kind = LogicalControlKind::ScratchClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = false},
      {.songTimeMicros = 200,
       .control = {.kind = LogicalControlKind::ScratchCounterClockwise,
                   .player = 1,
                   .lane = -1},
       .pressed = true},
  };
  std::string diagnostic;
  expect(adapter.applyBatch(std::span(batch).first(2), 250, diagnostic) &&
             adapter.applyBatch(std::span(batch).subspan(2), 250, diagnostic),
         "logical replay adapter accepts timestamp batches");
  expect(edges.size() == 4 && edges[0].lane == 10 &&
             edges[0].delay == 0.00015 && edges[1].lane == 7 &&
             !edges[2].pressed && edges[2].lane == 7 &&
             edges[2].backSpin && edges[3].pressed && edges[3].lane == 7,
         "adapter maps double lanes and same-time scratch reversal exactly");
  adapter.reset();
  expect(!edges.back().pressed && edges.back().lane == 10,
         "adapter reset releases every remaining physical lane");
}

void testMaterializerUsesDriverAndOnlyComparesSavedFacts() {
  const auto replay = document();
  const auto saved = savedResult();
  std::vector<std::int64_t> judgedInputs;
  bool finished = false;
  ReplayJudgingSink judge{
      .advanceTo = [](std::int64_t, std::string &) { return true; },
      .applyInput = [&](const InputTransition &event, std::string &) {
        judgedInputs.push_back(event.songTimeMicros);
        return true;
      },
      .finish = [&](std::string &) {
        finished = true;
        return std::optional(saved);
      }};

  const auto matched = ReplayPlaybackMaterializer::materialize(
      replay, ReplaySetupSource::LocalCapture, saved, judge);
  expect(matched.state == ReplayPlaybackMaterializationState::Matched &&
             judgedInputs == std::vector<std::int64_t>{100, 200} && finished,
         "judged materialization consumes raw input through the shared driver");

  auto different = saved;
  ++different.score.good;
  different.resultFingerprint =
      result_persistence::modernResultFingerprint(different);
  judge.finish = [&](std::string &) { return std::optional(different); };
  judgedInputs.clear();
  const auto mismatch = ReplayPlaybackMaterializer::materialize(
      replay, ReplaySetupSource::LocalCapture, saved, judge);
  expect(mismatch.state == ReplayPlaybackMaterializationState::ResultMismatch &&
             mismatch.agreement && !mismatch.agreement->agrees() &&
             saved.score.good != different.score.good,
         "materialized facts are compared and never replace saved result facts");
}

void testMaterializationBudgetStopsBeforeResultConstruction() {
  const auto replay = document();
  const auto saved = savedResult();
  bool finished = false;
  ReplayJudgingSink judge{
      .advanceTo = [](std::int64_t, std::string &) { return true; },
      .applyInput = [](const InputTransition &, std::string &) { return true; },
      .finish = [&](std::string &) {
        finished = true;
        return std::optional(saved);
      }};
  const auto bounded = ReplayPlaybackMaterializer::materialize(
      replay, ReplaySetupSource::LocalCapture, saved, judge, 1);
  expect(bounded.state ==
             ReplayPlaybackMaterializationState::WorkLimitExceeded &&
             !finished,
         "bounded materialization cannot construct facts from a partial replay");
}

} // namespace

int main() {
  testDriverMergesStreamsWithoutChangingTheirTiming();
  testDriverRejectsReverseTimeAndBoundsEachAdvance();
  testLogicalGameplayAdapterOwnsLaneAndScratchMapping();
  testMaterializerUsesDriverAndOnlyComparesSavedFacts();
  testConcreteMaterializerBuildsConsumerTrackOnlyAfterAgreement();
  testMaterializationBudgetStopsBeforeResultConstruction();
  if (failures != 0) {
    std::cerr << failures << " replay playback driver test(s) failed\n";
    return 1;
  }
  std::cout << "replay playback driver tests passed\n";
  return 0;
}
