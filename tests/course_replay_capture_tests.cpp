#if __has_include("replay/CourseReplayCapture.h")
#include "replay/CourseReplayCapture.h"
#include "replay/ReplaySetupProvenance.h"
#include "CourseConstraintUtils.h"
#define ASOBMASHOW_HAS_COURSE_REPLAY_CAPTURE 1
#else
#define ASOBMASHOW_HAS_COURSE_REPLAY_CAPTURE 0
#endif

#include <iostream>
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

#if ASOBMASHOW_HAS_COURSE_REPLAY_CAPTURE

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
  input.inputDevices = {InputDeviceCategory::Keyboard};
  return makeScoreProvenance(input);
}

result_persistence::ModernCourseStageResult
stage(int index, char hash, int keyMode, int maximumCombo, float gauge) {
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

result_persistence::ModernCourseResultCapture resultCapture() {
  return {
      .attemptId = "123e4567-e89b-42d3-a456-426614174000",
      .courseKey = "course:v1:" + repeated('c', 64),
      .legacyCourseId = 42,
      .courseName = "Capture Course",
      .courseGroupName = "Tests",
      .constraintJson = "[\"no_speed\"]",
      .requestedPlayOption = "NORMAL",
      .assistOption = "OFF",
      .initialGaugeType = GaugeType::Hard,
      .gaugeProfile = GaugeProfile::Standard,
      .gaugeAutoShift = GaugeAutoShiftMode::None,
      .gaugeAutoShiftLowerBound = GaugeType::AssistedEasy,
      .longNoteMode = 1,
      .clearType = kClearTypeHardClearRank,
      .stages = {stage(0, 'a', 7, 4, 76.0F), stage(1, 'b', 14, 8, 62.5F)},
      .entryFacts = {{.totalNotes = 5, .playLengthMicros = 1'000'000},
                     {.totalNotes = 5, .playLengthMicros = 2'000'000},
                     {.totalNotes = 5, .playLengthMicros = 3'000'000}},
      .playedAtUnixMillis = 1'700'000'000'456LL,
  };
}

replay::ReplayPlaybackData
playback(const result_persistence::ModernCourseStageResult &saved) {
  replay::LocalReplaySetupFacts facts{
      .chart = {.md5 = saved.score.chartMd5,
                .sha256 = saved.score.chartSha256,
                .keyMode = saved.keyMode},
      .longNoteMode =
          result_persistence::replaySetupLongNoteMode(saved.score)
              .value_or(-1),
  };
  std::string diagnostic;
  auto setup = replay::captureLocalReplaySetup(facts, saved.score.provenance,
                                               diagnostic);
  expect(setup.has_value(), "stage setup fixture captures");
  replay::ReplayPlaybackData value;
  if (setup) {
    value.setup = *setup;
  }
  value.input = {
      {.songTimeMicros = 0,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true},
      {.songTimeMicros = 1,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = false},
  };
  return value;
}

void testResultCaptureDerivesPartialAggregateFromOrderedFacts() {
  std::string diagnostic;
  const auto captured = result_persistence::captureModernCourseResult(
      resultCapture(), diagnostic);
  const std::vector<ScoreProvenance> expectedProvenance{
      captured ? captured->stages[0].score.provenance : ScoreProvenance{},
      captured ? captured->stages[1].score.provenance : ScoreProvenance{},
  };
  expect(captured.has_value(), "partial course facts capture a modern result");
  expect(captured && captured->completedCharts == 2 &&
             captured->totalCharts == 3 && captured->finalScore == 14 &&
             captured->maxScore == 30 && captured->maxCombo == 8 &&
             captured->finalGauge == 62.5F &&
             captured->provenance ==
                 mergeCourseProvenance(expectedProvenance) &&
             !captured->resultFingerprint.empty(),
         "course aggregate and fingerprint come from the ordered prefix");
  expect(captured && result_persistence::validateModernCourseResult(*captured,
                                                             diagnostic),
         "captured course result passes the strict reader validator");

  auto malformed = resultCapture();
  malformed.stages[1].stageIndex = 0;
  expect(!result_persistence::captureModernCourseResult(malformed, diagnostic),
         "non-contiguous completion facts cannot be captured");

  auto repeatedChart = resultCapture();
  repeatedChart.stages[1].score.chartMd5 =
      repeatedChart.stages[0].score.chartMd5;
  repeatedChart.stages[1].score.chartSha256 =
      repeatedChart.stages[0].score.chartSha256;
  repeatedChart.stages[1].score.provenance =
      repeatedChart.stages[0].score.provenance;
  expect(
      result_persistence::captureModernCourseResult(repeatedChart, diagnostic)
             .has_value(),
         "repeated chart identities remain valid at distinct stage indices");
}

void testRawCaptureDropsOnlyReplayAttachment() {
  std::string diagnostic;
  const auto result = result_persistence::captureModernCourseResult(
      resultCapture(), diagnostic);
  expect(result.has_value(), "result fixture captures");
  if (!result) {
    return;
  }

  replay::CourseReplayCapture capture{
      .result = *result,
      .stages =
          {
          {.playback = playback(result->stages[0]),
           .timeBounds = {.completionSongTimeMicros = 5'000'000},
           .restMicrosAfterStage =
               replay::kReplayLimits.maxCourseRestMicros},
          {.playback = playback(result->stages[1]),
           .timeBounds = {.completionSongTimeMicros = 6'000'000},
           .restMicrosAfterStage = 0},
      },
      .constraints = {.beatorajaConstraintIds = {4}, .longNoteMode = 1},
  };
  const auto accepted = replay::captureCourseReplayAttempt(capture, diagnostic);
  expect(accepted && accepted->result == *result && accepted->replay &&
             accepted->replay->playback.stages.size() == 2 &&
             accepted->pathInput.stageSha256 ==
                 std::vector<std::string>(
                     {repeated('a', 64), repeated('b', 64)}) &&
             accepted->pathInput.beatorajaConstraintIds ==
                 std::vector<int>({4}),
         "course capture retains canonical raw stages and path identity");
  expect(accepted && accepted->replay &&
             accepted->replay->playback.stages.front().setup.longNoteMode ==
                 1 &&
             accepted->result.stages.front().score.longNoteMode == 0,
         "course no-LN score buckets retain the actual stage setup mode");

  capture.stages[0].timeBounds = {.completionSongTimeMicros = 0};
  const auto acceptedAfterLateInput =
      replay::captureCourseReplayAttempt(capture, diagnostic);
  expect(acceptedAfterLateInput && acceptedAfterLateInput->replay &&
             acceptedAfterLateInput->replay->timeBounds.front() ==
                 replay::ReplayTimeBounds{.completionSongTimeMicros = 1},
         "course stage completion bounds include its last accepted input");
  capture.stages[0].timeBounds = {.completionSongTimeMicros = 5'000'000};

  capture.stages[1].playback.reset();
  const auto missing = replay::captureCourseReplayAttempt(capture, diagnostic);
  expect(missing && missing->result == *result && !missing->replay,
         "missing raw stage drops only the BRD attachment");

  capture.stages[1].playback = playback(result->stages[1]);
  capture.stages[0].restMicrosAfterStage =
      replay::kReplayLimits.maxCourseRestMicros + 1;
  const auto invalid = replay::captureCourseReplayAttempt(capture, diagnostic);
  expect(invalid && invalid->result == *result && invalid->replay,
         "capture leaves course envelope validation to the BRD codec");

  capture.stages[0].restMicrosAfterStage = 0;
  capture.constraints.beatorajaConstraintIds = {9};
  const auto wrongConstraints =
      replay::captureCourseReplayAttempt(capture, diagnostic);
  expect(wrongConstraints && wrongConstraints->result == *result &&
             wrongConstraints->replay,
         "capture leaves result and path binding to persistence");
}

void testBeatorajaConstraintIdentityIsCanonical() {
  const auto identifiers = beatorajaCourseConstraintIdsFromJson(
      R"(["hcn", "grade", "no-speed", "hcn", "gauge_7k", "unknown"])");
  expect(identifiers == std::vector<int>({1, 4, 9, 14}),
         "course replay path uses sorted unique Beatoraja constraint ids");
  expect(beatorajaCourseConstraintIdsFromJson("not-json").empty(),
         "malformed constraint metadata cannot invent replay identity");
}

void testSkinCourseTitlesUseBeatorajaPropertyBounds() {
  CoursePlaySession session;
  for (int index = 0; index < 12; ++index) {
    CoursePlayEntry entry;
    entry.meta.Title = "Stage " + std::to_string(index + 1);
    session.entries.push_back(std::move(entry));
  }

  const auto titles = session.beatorajaSkinStageTitles();
  expect(titles.size() == 10 && titles.front() == "Stage 1" &&
             titles.back() == "Stage 10",
         "skin course-title snapshots retain only Beatoraja's ten exposed "
         "course-title properties");
}

#endif

} // namespace

int main() {
#if ASOBMASHOW_HAS_COURSE_REPLAY_CAPTURE
  testResultCaptureDerivesPartialAggregateFromOrderedFacts();
  testRawCaptureDropsOnlyReplayAttachment();
  testBeatorajaConstraintIdentityIsCanonical();
  testSkinCourseTitlesUseBeatorajaPropertyBounds();
#else
  expect(false, "CourseReplayCapture contract is not implemented");
#endif
  if (failures != 0) {
    std::cerr << failures << " course replay capture test(s) failed\n";
    return 1;
  }
  std::cout << "course replay capture tests passed\n";
  return 0;
}
