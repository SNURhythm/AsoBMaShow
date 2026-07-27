#include "replay/ReplayPlaybackDriver.h"
#include "replay/ReplayPlaybackMaterializer.h"

#include "ScoreProvenance.h"

#include <iostream>
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

void testDriverMergesStreamsWithoutChangingTheirTiming() {
  ReplayPlaybackDriver driver(document(), ReplaySetupSource::LocalCapture);
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
  ReplayPlaybackDriver driver(document(), ReplaySetupSource::LocalCapture);
  ReplayPlaybackSink sink;
  expect(driver.advanceTo(200, sink).state ==
             ReplayPlaybackDriverState::Advanced,
         "first monotonic advance succeeds with optional auxiliary sinks");
  expect(driver.advanceTo(199, sink).state ==
             ReplayPlaybackDriverState::NonMonotonicAdvance,
         "reverse playback time fails closed");

  ReplayPlaybackDriver bounded(document(), ReplaySetupSource::LocalCapture);
  const auto exhausted = bounded.advanceTo(1'000, sink, 2);
  expect(exhausted.state == ReplayPlaybackDriverState::WorkLimitExceeded &&
             !bounded.complete(),
         "per-run event budget bounds adversarial playback work");
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
  testMaterializerUsesDriverAndOnlyComparesSavedFacts();
  testMaterializationBudgetStopsBeforeResultConstruction();
  if (failures != 0) {
    std::cerr << failures << " replay playback driver test(s) failed\n";
    return 1;
  }
  std::cout << "replay playback driver tests passed\n";
  return 0;
}
