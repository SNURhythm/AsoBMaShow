// The conditional keeps the red contract buildable before production exists.
#if __has_include("replay/CourseReplayConsumer.h")
#include "replay/CourseReplayConsumer.h"
#include "CoursePlaySession.h"
#define ASOBMASHOW_HAS_COURSE_REPLAY_CONSUMER 1
#else
#define ASOBMASHOW_HAS_COURSE_REPLAY_CONSUMER 0
#endif

#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

#if ASOBMASHOW_HAS_COURSE_REPLAY_CONSUMER

using namespace replay;

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

ScoreProvenance provenance(char hash, int keyMode) {
  ScoreProvenanceBuildInput input;
  input.chartMeta.MD5 = repeated(hash, 32);
  input.chartMeta.SHA256 = repeated(hash, 64);
  input.chartMeta.KeyMode = keyMode;
  input.chartMeta.Rank = 2;
  input.chartMeta.TotalNotes = 5;
  input.chartMeta.HasTotal = true;
  input.chartMeta.Total = 200.0;
  input.longNoteMode = 1;
  input.sourceJudgeRank = 2;
  input.effectiveJudgeWindows = {
      {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
      {Good, {-75'000, 75'000}},   {Bad, {-200'000, 200'000}},
      {Kpoor, {-1'000'000, 0}},
  };
  input.totalNotes = 5;
  input.authoredGaugeTotal = 200.0;
  input.effectiveGaugeTotal = 200.0;
  input.gaugeType = GaugeType::Hard;
  input.gaugeAutoShift = GaugeAutoShiftMode::Continue;
  input.gaugeAutoShiftLowerBound = GaugeType::Easy;
  input.inputDevices = {InputDeviceCategory::Keyboard};
  return makeScoreProvenance(input);
}

result_persistence::ModernCourseStageResult stage(int index, char hash,
                                                   int keyMode,
                                                   int maximumCombo,
                                                   float gauge) {
  result_persistence::ModernCourseStageResult value;
  value.stageIndex = index;
  value.score.chartPath = "library/stage-" + std::to_string(index) + ".bms";
  value.score.chartMd5 = repeated(hash, 32);
  value.score.chartSha256 = repeated(hash, 64);
  value.score.chartTitle = "Stage";
  value.score.chartArtist = "Artist";
  value.score.longNoteMode = 0;
  value.score.score = 7;
  value.score.maxScore = 10;
  value.score.maxCombo = maximumCombo;
  value.score.comboBreak = 1;
  value.score.pGreat = 3;
  value.score.great = 1;
  value.score.good = 1;
  value.score.finalGauge = gauge;
  value.score.clearType = kClearTypeHardClearRank;
  value.score.provenance = provenance(hash, keyMode);
  value.keyMode = keyMode;
  value.adoptedGaugeType = GaugeType::Hard;
  value.adoptedGaugeHistory = {80.0F, gauge};
  return value;
}

result_persistence::ModernCourseResult savedResult() {
  result_persistence::ModernCourseResultCapture capture{
      .attemptId = "123e4567-e89b-42d3-a456-426614174000",
      .courseKey = "course:v1:" + repeated('c', 64),
      .legacyCourseId = 42,
      .courseName = "Consumer Course",
      .courseGroupName = "Tests",
      .constraintJson = R"(["no_speed","gauge_7k"])",
      .requestedPlayOption = "NORMAL",
      .assistOption = "OFF",
      .initialGaugeType = GaugeType::Hard,
      .gaugeProfile = GaugeProfile::Standard,
      .gaugeAutoShift = GaugeAutoShiftMode::Continue,
      .gaugeAutoShiftLowerBound = GaugeType::Easy,
      .longNoteMode = 1,
      .clearType = kClearTypeHardClearRank,
      .stages = {stage(0, 'a', 7, 4, 76.0F),
                 stage(1, 'b', 14, 8, 62.5F)},
      .entryFacts = {{.totalNotes = 5, .playLengthMicros = 1'000'000},
                     {.totalNotes = 5, .playLengthMicros = 2'000'000},
                     {.totalNotes = 5, .playLengthMicros = 3'000'000}},
      .playedAtUnixMillis = 1'700'000'000'456LL,
  };
  std::string diagnostic;
  auto result = result_persistence::captureModernCourseResult(capture,
                                                               diagnostic);
  expect(result.has_value(), "consumer result fixture captures");
  if (!result) {
    return {};
  }
  result->resultId = 17;
  return *result;
}

ReplaySetup setup(const result_persistence::ModernCourseResult &result,
                  std::size_t index) {
  const auto &saved = result.stages[index];
  ReplaySetup value;
  value.chart = {.md5 = saved.score.chartMd5,
                 .sha256 = saved.score.chartSha256,
                 .keyMode = saved.keyMode};
  value.longNoteMode =
      result_persistence::replaySetupLongNoteMode(saved.score).value_or(-1);
  value.player1.option = result.requestedPlayOption;
  value.assistOption = result.assistOption;
  value.initialGaugeType = result.initialGaugeType;
  value.gaugeProfile = result.gaugeProfile;
  value.gaugeAutoShift = result.gaugeAutoShift;
  value.gaugeAutoShiftLowerBound = result.gaugeAutoShiftLowerBound;
  value.ruleset = saved.score.provenance.ruleset;
  value.playback = saved.score.provenance.playback;
  value.candidateSelection =
      saved.score.provenance.stages.front().candidateSelection;
  value.judgeWindowScalePercent =
      saved.score.provenance.judgeWindowScalePercent;
  return value;
}

ReplayCourseDocument document(
    const result_persistence::ModernCourseResult &result) {
  ReplayCourseDocument value;
  for (std::size_t index = 0; index < result.stages.size(); ++index) {
    value.playback.stages.push_back({.setup = setup(result, index)});
    value.playback.restMicrosAfterStage.push_back(index == 0 ? 1'000'000 : 0);
    value.timeBounds.push_back({.completionSongTimeMicros = 5'000'000});
  }
  return value;
}

std::unique_ptr<bms_parser::Chart> chartFor(
    const result_persistence::ModernCourseStageResult &stage) {
  auto chart = std::make_unique<bms_parser::Chart>();
  chart->Meta.BmsPath = stage.score.chartPath;
  chart->Meta.MD5 = stage.score.chartMd5;
  chart->Meta.SHA256 = stage.score.chartSha256;
  chart->Meta.KeyMode = stage.keyMode;
  chart->Meta.LnMode =
      result_persistence::replaySetupLongNoteMode(stage.score).value_or(-1);
  chart->Meta.TotalNotes = 5;
  return chart;
}

result_persistence::ModernChartResult chartResultFor(
    const result_persistence::ModernCourseResult &course,
    std::size_t index) {
  const auto &stage = course.stages[index];
  result_persistence::ModernChartResult result{
      .attemptId = course.attemptId,
      .score = stage.score,
      .keyMode = stage.keyMode,
      .adoptedGaugeType = stage.adoptedGaugeType,
      .adoptedGaugeHistory = stage.adoptedGaugeHistory,
      .judgementTiming = stage.judgementTiming,
      .playedAtUnixMillis = course.playedAtUnixMillis,
  };
  result.resultFingerprint = result_persistence::modernResultFingerprint(result);
  return result;
}

GaugeStateSnapshot gauge(float current) {
  GaugeStateSnapshot value;
  value.gaugeType = GaugeType::Hard;
  value.selectedGaugeType = GaugeType::Hard;
  value.gaugeAutoShiftLowerBound = GaugeType::Easy;
  value.gaugeProfile = GaugeProfile::Standard;
  value.gaugeAutoShift = GaugeAutoShiftMode::Continue;
  value.currentGauge = current;
  value.gaugeValues.fill(current);
  return value;
}

struct ConsumerHarness {
  ModernCourseResultRecord listed{.result = savedResult()};
  ReplayCourseDocument replay = document(listed.result);
  std::vector<std::string> calls;
  bool contextReady = true;
  bool disagreeingCarriedGauge = false;

  CourseReplayConsumer makeConsumer() {
    return CourseReplayConsumer({
        .parseBaseChart = [this](const std::filesystem::path &path,
                                 const ReplayChartIdentity &identity,
                                 const ScoreProvenance &provenance,
                                 std::atomic_bool &, std::string &) {
          const std::size_t index = path.filename() == "stage-0.bms" ? 0 : 1;
          calls.push_back("parse-" + std::to_string(index));
          expect(identity.sha256 ==
                     listed.result.stages[index].score.chartSha256 &&
                     provenance ==
                         listed.result.stages[index].score.provenance,
                 "consumer parses each stage on its saved random branch");
          return chartFor(listed.result.stages[index]);
        },
        .loadContext = [this](std::string_view attemptId,
                              const ParsedCourseReplayFacts &facts) {
          calls.emplace_back("context");
          expect(attemptId == listed.result.attemptId &&
                     facts.stages.size() == listed.result.stages.size() &&
                     facts.stages[0].chart.sha256 ==
                         listed.result.stages[0].score.chartSha256 &&
                     facts.stages[1].chart.keyMode ==
                         listed.result.stages[1].keyMode,
                 "consumer supplies the exact parsed completed prefix");
          if (!contextReady) {
            return CourseReplayContextOutcome{
                .state = CourseReplayContextState::FileMissing,
                .result = listed.result,
            };
          }
          VerifiedCourseReplay verified{
              .result = listed.result,
              .document = replay,
              .stageSources = {ReplayStageDecodeSource::AsoExtension,
                               ReplayStageDecodeSource::AsoExtension},
          };
          return CourseReplayContextOutcome{
              .state = CourseReplayContextState::Ready,
              .result = listed.result,
              .verified = std::move(verified),
          };
        },
        .prepareChart = [this](const std::filesystem::path &path,
                               const ReplaySetup &stageSetup,
                               const ScoreProvenance &stageProvenance,
                               const bms_parser::ChartMeta &parsedMeta,
                               std::atomic_bool &, std::string &) {
          const std::size_t index = path.filename() == "stage-0.bms" ? 0 : 1;
          calls.push_back("prepare-" + std::to_string(index));
          expect(stageSetup == replay.playback.stages[index].setup &&
                     stageProvenance ==
                         listed.result.stages[index].score.provenance &&
                     parsedMeta.SHA256 ==
                         listed.result.stages[index].score.chartSha256,
                 "consumer prepares each stage from verified setup only");
          return chartFor(listed.result.stages[index]);
        },
        .materializeStage = [this](
                                const ReplayChartDocument &stageDocument,
                                ReplaySetupSource source,
                                const result_persistence::ModernChartResult &saved,
                                const bms_parser::Chart &,
                                const ReplayPlaybackCarryState &carry) {
          const std::size_t index = saved.score.chartSha256.front() == 'a' ? 0 : 1;
          calls.push_back("materialize-" + std::to_string(index));
          expect(stageDocument.playback == replay.playback.stages[index] &&
                     source == ReplaySetupSource::AsoExtension,
                 "consumer materializes only verified raw stage input");
          if (index == 0) {
            expect(!carry.gauge.has_value() && carry.combo == 0 &&
                       carry.maximumCombo == 0,
                   "first stage starts without invented carried state");
          } else {
            expect(carry.gauge.has_value() &&
                       carry.gauge->currentGauge == 76.0F &&
                       carry.combo == 3 && carry.maximumCombo == 4,
                   "second stage receives the shared continuation state");
          }
          ReplayPlaybackMaterializationOutcome outcome{
              .state = ReplayPlaybackMaterializationState::Matched,
              .judgedResult = saved,
              .initialGaugeState = index == 0
                                       ? std::optional(gauge(80.0F))
                                       : carry.gauge,
              .finalGaugeState = gauge(
                  index == 0 ? 76.0F
                             : (disagreeingCarriedGauge ? 50.0F : 62.5F)),
              .endingCombo = index == 0 ? 3 : 0,
              .replayData = std::make_shared<ReplayData>(),
          };
          return outcome;
        },
    });
  }
};

void testConsumerOwnsOneVerifiedCoursePipelineAndContinuation() {
  ConsumerHarness harness;
  auto consumer = harness.makeConsumer();
  std::atomic_bool cancelled = false;
  const std::vector<std::filesystem::path> paths{
      "selected/stage-0.bms", "selected/stage-1.bms"};
  const auto loaded = consumer.load(harness.listed, paths, cancelled);
  expect(loaded.ready() && loaded.charts.size() == 2 &&
             loaded.materializedStages.size() == 2 &&
             loaded.materializedStages[0].finalGaugeState.currentGauge ==
                 76.0F &&
             loaded.materializedStages[1].initialGaugeState.currentGauge ==
                 76.0F &&
             loaded.replayData &&
             loaded.replayData->stages.size() == 2 && loaded.continuation &&
             loaded.continuation->complete() &&
             loaded.continuation->score == harness.listed.result.finalScore &&
             loaded.continuation->maximumCombo ==
                 harness.listed.result.maxCombo &&
             loaded.continuation->gauge.currentGauge ==
                 harness.listed.result.finalGauge,
         "verified stages yield one carried-state-checked compatibility course");
  expect(harness.calls ==
             std::vector<std::string>{"parse-0", "parse-1", "context",
                                      "prepare-0", "materialize-0",
                                      "prepare-1", "materialize-1"},
         "course consumer has one ordered parse/context/setup/judge pipeline");
}

void testReplayFailureStopsBeforeSetupAndProducesNoAdapter() {
  ConsumerHarness harness;
  harness.contextReady = false;
  auto consumer = harness.makeConsumer();
  std::atomic_bool cancelled = false;
  const std::vector<std::filesystem::path> paths{
      "selected/stage-0.bms", "selected/stage-1.bms"};
  const auto loaded = consumer.load(harness.listed, paths, cancelled);
  expect(loaded.state == CourseReplayConsumerState::ReplayUnavailable &&
             !loaded.ready() && loaded.charts.empty() && !loaded.replayData &&
             loaded.replayState() == ReplayState::Missing &&
             harness.calls ==
                 std::vector<std::string>{"parse-0", "parse-1", "context"},
         "missing course BRD stops before setup, judging, and adapters");
}

void testMaterializedCarriedStateMustAgreeWithSavedFacts() {
  ConsumerHarness harness;
  harness.disagreeingCarriedGauge = true;
  auto consumer = harness.makeConsumer();
  std::atomic_bool cancelled = false;
  const std::vector<std::filesystem::path> paths{
      "selected/stage-0.bms", "selected/stage-1.bms"};
  const auto loaded = consumer.load(harness.listed, paths, cancelled);
  expect(loaded.state == CourseReplayConsumerState::ResultMismatch &&
             !loaded.ready() && !loaded.replayData &&
             loaded.replayState() == ReplayState::Mismatched,
         "materialized carried gauge disagreement fails closed");
}

void testVerifiedLaunchAdaptersSeparateWatchFromRetrySame() {
  const std::vector<std::filesystem::path> paths{
      "selected/stage-0.bms", "selected/stage-1.bms"};

  ConsumerHarness watchHarness;
  auto watchConsumer = watchHarness.makeConsumer();
  std::atomic_bool cancelled = false;
  auto watchLoaded =
      watchConsumer.load(watchHarness.listed, paths, cancelled);
  auto watch = makeCourseReplayLaunchSession(
      std::move(watchLoaded), CourseReplayLaunchMode::Watch, true, true);
  expect(watch && watch->courseReplayPlayback && watch->courseReplayData &&
             !watch->courseRetrySameData &&
             watch->entries.size() == 3 &&
             watch->entries[2].meta.TotalNotes == 5 &&
             watch->preparedCourseCharts.size() == 2 &&
             watch->replayTouchVisualizationEnabled == true &&
             watch->replayGhostRenderingEnabled == true,
         "Watch adapter retains verified playback and prepared charts");

  ConsumerHarness retryHarness;
  auto retryConsumer = retryHarness.makeConsumer();
  cancelled = false;
  auto retryLoaded =
      retryConsumer.load(retryHarness.listed, paths, cancelled);
  auto retry = makeCourseReplayLaunchSession(
      std::move(retryLoaded), CourseReplayLaunchMode::RetrySame);
  expect(retry && !retry->courseReplayPlayback && !retry->courseReplayData &&
             retry->courseRetrySameData &&
             retry->courseRetrySameStageSetup(0) != nullptr &&
             retry->preparedCourseCharts.size() == 2,
         "Retry Same adapter keeps validated setup without replay playback");
}

#endif

} // namespace

int main() {
#if ASOBMASHOW_HAS_COURSE_REPLAY_CONSUMER
  testConsumerOwnsOneVerifiedCoursePipelineAndContinuation();
  testReplayFailureStopsBeforeSetupAndProducesNoAdapter();
  testMaterializedCarriedStateMustAgreeWithSavedFacts();
  testVerifiedLaunchAdaptersSeparateWatchFromRetrySame();
#else
  expect(false, "CourseReplayConsumer contract is not implemented");
#endif
  if (failures != 0) {
    std::cerr << failures << " course replay consumer test(s) failed\n";
    return 1;
  }
  std::cout << "course replay consumer tests passed\n";
  return 0;
}
