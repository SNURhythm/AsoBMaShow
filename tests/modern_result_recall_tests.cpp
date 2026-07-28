#include "ModernResultRecallBuilder.h"

#include "LongNoteModeUtils.h"
#include "replay/ReplayCapabilities.h"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kAttemptId = "123e4567-e89b-42d3-a456-426614174000";

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

result_persistence::ChartScoreWrite scoreFor(int index = 0) {
  result_persistence::ChartScoreWrite score;
  score.chartPath = "library/stage-" + std::to_string(index) + ".bms";
  score.chartMd5 = repeated(static_cast<char>('b' + index), 32);
  score.chartSha256 = repeated(static_cast<char>('a' + index), 64);
  score.chartTitle = "Saved title " + std::to_string(index);
  score.chartArtist = "Saved artist " + std::to_string(index);
  score.longNoteMode = 1;
  score.score = 7;
  score.maxScore = 10;
  score.maxCombo = index == 0 ? 4 : 8;
  score.comboBreak = 1;
  score.pGreat = 3;
  score.great = 1;
  score.good = 1;
  score.fast = 1;
  score.slow = 1;
  score.finalGauge = index == 0 ? 72.5F : 62.5F;
  score.clearType = kClearTypeNormalClearRank;
  score.provenance = ScoreProvenance::Legacy();
  return score;
}

result_persistence::ChartJudgementTiming timingForScore() {
  result_persistence::ChartJudgementTiming timing;
  timing.byJudgement[static_cast<std::size_t>(PGreat)] = {.fast = 1, .slow = 0};
  timing.byJudgement[static_cast<std::size_t>(Great)] = {.fast = 0, .slow = 1};
  return timing;
}

ScoreProvenance provenanceFor(
    const result_persistence::ChartScoreWrite &score,
    std::vector<int> randomValues) {
  ScoreProvenanceBuildInput input;
  input.chartMeta.MD5 = score.chartMd5;
  input.chartMeta.SHA256 = score.chartSha256;
  input.chartMeta.KeyMode = 7;
  input.chartMeta.Rank = 2;
  input.chartMeta.TotalNotes = score.maxScore / 2;
  input.chartMeta.HasTotal = true;
  input.chartMeta.Total = 200.0;
  input.chartMeta.RandomSeed = 42;
  input.chartMeta.RandomPrng = bms_parser::Parser::RandomPrngId;
  input.chartMeta.RandomValues = std::move(randomValues);
  input.longNoteMode = score.longNoteMode;
  input.sourceJudgeRank = 2;
  input.effectiveJudgeWindows = {
      {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
      {Good, {-75'000, 75'000}},   {Bad, {-200'000, 200'000}},
      {Kpoor, {-1'000'000, 0}},
  };
  input.totalNotes = score.maxScore / 2;
  input.authoredGaugeTotal = 200.0;
  input.effectiveGaugeTotal = 200.0;
  return makeScoreProvenance(input);
}

result_persistence::ModernChartResult chartResult() {
  result_persistence::ModernChartResult result;
  result.attemptId = std::string(kAttemptId);
  result.score = scoreFor();
  result.keyMode = 7;
  result.adoptedGaugeType = GaugeType::Normal;
  result.adoptedGaugeHistory = {20.0F, 48.0F, result.score.finalGauge};
  result.judgementTiming = timingForScore();
  result.playedAtUnixMillis = 1'700'000'000'123LL;
  result.resultFingerprint =
      result_persistence::modernResultFingerprint(result);
  return result;
}

result_persistence::ModernCourseResult courseResult() {
  result_persistence::ModernCourseResult result;
  result.attemptId = std::string(kAttemptId);
  result.courseKey = "course:v1:" + repeated('d', 64);
  result.legacyCourseId = 42;
  result.courseName = "Saved course";
  result.courseGroupName = "Saved group";
  result.constraintJson = "{}";
  result.completedCharts = 2;
  result.totalCharts = 3;
  result.requestedPlayOption = "NORMAL";
  result.assistOption = assist_options::kOff;
  result.initialGaugeType = GaugeType::Normal;
  result.gaugeProfile = GaugeProfile::CourseDefault;
  result.gaugeAutoShift = GaugeAutoShiftMode::Continue;
  result.gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  result.longNoteMode = 1;
  result.finalScore = 14;
  result.maxScore = 30;
  result.maxCombo = 8;
  result.finalGauge = 62.5F;
  result.clearType = kClearTypeNormalClearRank;
  result.provenance = ScoreProvenance::Legacy();
  result.entryFacts = {{.totalNotes = 5, .playLengthMicros = 1'000'000},
                       {.totalNotes = 5, .playLengthMicros = 2'000'000},
                       {.totalNotes = 5, .playLengthMicros = 3'000'000}};
  for (int index = 0; index < result.completedCharts; ++index) {
    result_persistence::ModernCourseStageResult stage;
    stage.stageIndex = index;
    stage.score = scoreFor(index);
    stage.keyMode = 7;
    stage.adoptedGaugeType = GaugeType::Normal;
    stage.adoptedGaugeHistory = {100.0F,
                                 index == 0 ? 72.5F : result.finalGauge};
    stage.judgementTiming = timingForScore();
    result.stages.push_back(std::move(stage));
  }
  result.playedAtUnixMillis = 1'700'000'001'234LL;
  result.resultFingerprint =
      result_persistence::modernResultFingerprint(result);
  return result;
}

std::unique_ptr<bms_parser::Chart>
parsedChartFor(const result_persistence::ChartScoreWrite &score,
               std::string title = "Parsed title") {
  auto chart = std::make_unique<bms_parser::Chart>();
  chart->Meta.BmsPath = score.chartPath;
  chart->Meta.SHA256 =
      "  " +
      repeated(static_cast<char>(score.chartSha256.front() - 'a' + 'A'), 64) +
      " ";
  chart->Meta.MD5 =
      " " +
      repeated(static_cast<char>(score.chartMd5.front() - 'a' + 'A'), 32) +
      "  ";
  chart->Meta.Title = std::move(title);
  chart->Meta.Artist = "Parsed artist";
  chart->Meta.KeyMode = 7;
  chart->Meta.TotalNotes = 5;
  return chart;
}

std::unique_ptr<bms_parser::Chart> parsedUndefinedLongNoteChartFor(
    const result_persistence::ChartScoreWrite &score,
    std::vector<int> randomValues, int rawTotalNotes) {
  auto chart = parsedChartFor(score);
  chart->Meta.LnMode = long_note_mode::kUnknownValue;
  chart->Meta.TotalNotes = rawTotalNotes;
  chart->Meta.RandomSeed = 42;
  chart->Meta.RandomPrng = bms_parser::Parser::RandomPrngId;
  chart->Meta.RandomValues = std::move(randomValues);

  auto *measure = new bms_parser::Measure();
  auto *headTimeline = new bms_parser::TimeLine(8, false);
  for (int lane = 0; lane < 4; ++lane) {
    headTimeline->SetNote(lane, new bms_parser::Note(lane + 1));
  }
  auto *tailTimeline = new bms_parser::TimeLine(8, false);
  auto *head = new bms_parser::LongNote(
      bms_parser::Parser::NoWav, bms_parser::LongNoteType::Undefined);
  auto *tail = new bms_parser::LongNote(
      bms_parser::Parser::NoWav, bms_parser::LongNoteType::Undefined);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(4, head);
  tailTimeline->SetNote(4, tail);
  measure->TimeLines.push_back(headTimeline);
  measure->TimeLines.push_back(tailTimeline);
  chart->Measures.push_back(measure);
  return chart;
}

void testChartRecallReappliesSavedRandomAndLongNoteSetup() {
  auto saved = chartResult();
  saved.score.provenance = provenanceFor(saved.score, {2, 1, 3});
  saved.resultFingerprint = result_persistence::modernResultFingerprint(saved);
  std::atomic_bool cancelled{false};

  const auto restored = result_recall::BuildChartResult(
      result_persistence::ModernChartResult(saved), cancelled,
      [&](const std::filesystem::path &,
          std::atomic_bool &) -> std::unique_ptr<bms_parser::Chart> {
        return parsedUndefinedLongNoteChartFor(saved.score, {2, 1, 3}, 6);
      });
  expect(restored.value && restored.value->chart->Meta.TotalNotes == 5 &&
             restored.value->chart->Meta.LnMode ==
                 long_note_mode::kLnValue,
         "chart recall applies the saved long-note mode before maximum-score "
         "agreement");

  const auto wrongBranch = result_recall::BuildChartResult(
      result_persistence::ModernChartResult(saved), cancelled,
      [&](const std::filesystem::path &,
          std::atomic_bool &) -> std::unique_ptr<bms_parser::Chart> {
        return parsedUndefinedLongNoteChartFor(saved.score, {1, 2, 3}, 5);
      });
  expect(!wrongBranch.value &&
             wrongBranch.diagnostic.find("setup") != std::string::npos,
         "equal-note-count random branches must agree with saved provenance");
}

void testChartRecallUsesOnlySavedFacts() {
  const auto saved = chartResult();
  std::atomic_bool cancelled{false};
  int loads = 0;
  std::filesystem::path requestedPath;
  auto loader = [&](const std::filesystem::path &path,
                    std::atomic_bool &) -> std::unique_ptr<bms_parser::Chart> {
    ++loads;
    requestedPath = path;
    return parsedChartFor(saved.score);
  };

  const auto outcome = result_recall::BuildChartResult(
      result_persistence::ModernChartResult(saved), cancelled, loader);
  expect(outcome.value.has_value(),
         "valid modern chart result recalls without replay data");
  expect(loads == 1 && requestedPath == saved.score.chartPath,
         "modern recall loads exactly the independently stored chart path");
  if (!outcome.value) {
    return;
  }
  const auto &view = *outcome.value;
  expect(view.result == saved,
         "recall publishes the validated modern result without mutation");
  expect(view.chart->Meta.Title == saved.score.chartTitle &&
             view.chart->Meta.Artist == saved.score.chartArtist,
         "stored display metadata is applied after identity agreement");
  expect(view.state.getScore() == saved.score.score &&
             view.state.maxCombo == saved.score.maxCombo &&
             view.state.comboBreak == saved.score.comboBreak &&
             view.state.judgeCount.at(PGreat) == saved.score.pGreat &&
             view.state.judgeCount.at(Great) == saved.score.great &&
             view.state.judgeCount.at(Good) == saved.score.good &&
             view.state.fastCount == saved.score.fast &&
             view.state.slowCount == saved.score.slow,
         "recall reconstructs score presentation solely from saved facts");
  expect(view.state.gaugeType == saved.adoptedGaugeType &&
             view.state.currentGauge == saved.score.finalGauge &&
             view.state.gaugeHistoryFor(saved.adoptedGaugeType) ==
                 saved.adoptedGaugeHistory &&
             view.state.judgementFastSlowCount.at(PGreat).fast == 1 &&
             view.state.judgementFastSlowCount.at(Great).slow == 1,
         "recall restores adopted gauge and judgement timing presentation");

  const auto expectedFingerprint = view.result.resultFingerprint;
  const std::vector<replay::ReplayState> replayStates{
      replay::ReplayState::Verified, replay::ReplayState::Missing,
      replay::ReplayState::Corrupt, replay::ReplayState::UserDeleted};
  for (const auto ignoredReplayState : replayStates) {
    (void)ignoredReplayState;
    const auto repeatedOutcome = result_recall::BuildChartResult(
        result_persistence::ModernChartResult(saved), cancelled, loader);
    expect(repeatedOutcome.value &&
               repeatedOutcome.value->result.resultFingerprint ==
                   expectedFingerprint &&
               repeatedOutcome.value->state.getScore() == saved.score.score,
           "replay-file lifecycle state cannot affect modern result recall");
  }
}

void testChartRecallFailsClosedBeforePublishing() {
  std::atomic_bool cancelled{false};
  int loads = 0;
  auto invalid = chartResult();
  invalid.resultFingerprint.front() =
      invalid.resultFingerprint.front() == '0' ? '1' : '0';
  auto loader = [&](const std::filesystem::path &,
                    std::atomic_bool &) -> std::unique_ptr<bms_parser::Chart> {
    ++loads;
    return parsedChartFor(invalid.score);
  };
  auto outcome =
      result_recall::BuildChartResult(std::move(invalid), cancelled, loader);
  expect(!outcome.value && loads == 0,
         "malformed modern results are rejected before chart loading");

  const auto saved = chartResult();
  outcome = result_recall::BuildChartResult(
      result_persistence::ModernChartResult(saved), cancelled,
      [&](const std::filesystem::path &,
          std::atomic_bool &) -> std::unique_ptr<bms_parser::Chart> {
        ++loads;
        auto chart = parsedChartFor(saved.score, "Untrusted title");
        chart->Meta.SHA256 = repeated('f', 64);
        return chart;
      });
  expect(!outcome.value &&
             outcome.diagnostic.find("identity") != std::string::npos,
         "parsed identity mismatch publishes no stored display metadata");

  outcome = result_recall::BuildChartResult(
      result_persistence::ModernChartResult(saved), cancelled,
      [](const std::filesystem::path &, std::atomic_bool &)
          -> std::unique_ptr<bms_parser::Chart> { return nullptr; });
  expect(!outcome.value &&
             outcome.diagnostic.find("unavailable") != std::string::npos,
         "missing chart content disables only rich result recall");
}

void testMovedChartRecallUsesCurrentLocationAndSavedIdentity() {
  const auto saved = chartResult();
  const std::filesystem::path currentPath =
      "moved-library/current-stage.bms";
  std::atomic_bool cancelled{false};
  std::filesystem::path requestedPath;
  const auto outcome = result_recall::BuildChartResult(
      result_persistence::ModernChartResult(saved), cancelled, currentPath,
      [&](const std::filesystem::path &path,
          std::atomic_bool &) -> std::unique_ptr<bms_parser::Chart> {
        requestedPath = path;
        return parsedChartFor(saved.score);
      });
  expect(outcome.value && requestedPath == currentPath,
         "moved chart recall loads the current Records path and validates it "
         "against saved identity facts");
}

void testCourseRecallIsOrderedCompleteAndAtomic() {
  const auto saved = courseResult();
  std::atomic_bool cancelled{false};
  std::vector<std::filesystem::path> requestedPaths;
  auto loader = [&](const std::filesystem::path &path,
                    std::atomic_bool &) -> std::unique_ptr<bms_parser::Chart> {
    requestedPaths.push_back(path);
    const int index = path == saved.stages.front().score.chartPath ? 0 : 1;
    return parsedChartFor(saved.stages[static_cast<std::size_t>(index)].score);
  };

  const auto outcome = result_recall::BuildCourseResult(
      result_persistence::ModernCourseResult(saved), cancelled, loader);
  expect(outcome.value.has_value(),
         "valid partial modern course result recalls without playback");
  expect(requestedPaths.size() == 2 &&
             requestedPaths[0] == saved.stages[0].score.chartPath &&
             requestedPaths[1] == saved.stages[1].score.chartPath,
         "course recall loads the completed prefix in stored order");
  if (outcome.value) {
    expect(outcome.value->completedStages.size() == 2 &&
               outcome.value->result.entryFacts.size() == 3,
           "course recall retains completed stage state and full suffix facts");
    expect(outcome.value->completedStages[0].state.maxCombo == 4 &&
               outcome.value->completedStages[1].state.maxCombo == 8 &&
               outcome.value->completedStages[1].state.currentGauge == 62.5F,
           "course recall uses ordered per-stage saved result facts");
  }

  requestedPaths.clear();
  const auto mismatch = result_recall::BuildCourseResult(
      result_persistence::ModernCourseResult(saved), cancelled,
      [&](const std::filesystem::path &path,
          std::atomic_bool &) -> std::unique_ptr<bms_parser::Chart> {
        requestedPaths.push_back(path);
        const int index = path == saved.stages.front().score.chartPath ? 0 : 1;
        auto chart =
            parsedChartFor(saved.stages[static_cast<std::size_t>(index)].score);
        if (index == 1) {
          chart->Meta.MD5 = repeated('f', 32);
        }
        return chart;
      });
  expect(!mismatch.value && requestedPaths.size() == 2,
         "a later course identity mismatch publishes no partial result");

  auto malformed = saved;
  malformed.stages[1].stageIndex = 0;
  malformed.resultFingerprint =
      result_persistence::modernResultFingerprint(malformed);
  requestedPaths.clear();
  const auto malformedOutcome =
      result_recall::BuildCourseResult(std::move(malformed), cancelled, loader);
  expect(!malformedOutcome.value && requestedPaths.empty(),
         "malformed course ordering is rejected before any chart load");
}

void testCourseRecallReappliesEachStageSetupBeforeAgreement() {
  auto saved = courseResult();
  std::vector<ScoreProvenance> stageProvenance;
  for (std::size_t index = 0; index < saved.stages.size(); ++index) {
    auto &score = saved.stages[index].score;
    score.provenance =
        provenanceFor(score, {static_cast<int>(index) + 1, 3});
    stageProvenance.push_back(score.provenance);
  }
  saved.provenance = mergeCourseProvenance(stageProvenance);
  saved.resultFingerprint = result_persistence::modernResultFingerprint(saved);

  std::atomic_bool cancelled{false};
  const auto restored = result_recall::BuildCourseResult(
      result_persistence::ModernCourseResult(saved), cancelled,
      [&](const std::filesystem::path &path,
          std::atomic_bool &) -> std::unique_ptr<bms_parser::Chart> {
        const std::size_t index =
            path == saved.stages.front().score.chartPath ? 0 : 1;
        return parsedUndefinedLongNoteChartFor(
            saved.stages[index].score,
            {static_cast<int>(index) + 1, 3}, 6);
      });
  expect(restored.value && restored.value->completedStages.size() == 2 &&
             restored.value->completedStages[0].chart->Meta.TotalNotes == 5 &&
             restored.value->completedStages[1].chart->Meta.TotalNotes == 5,
         "course recall applies each saved stage setup before publishing the "
         "ordered result");
}

void testMovedCourseRecallUsesCurrentOrderedLocations() {
  const auto saved = courseResult();
  const std::vector<std::filesystem::path> currentPaths{
      "moved-course/current-stage-0.bms",
      "moved-course/current-stage-1.bms"};
  std::atomic_bool cancelled{false};
  std::vector<std::filesystem::path> requestedPaths;
  const auto outcome = result_recall::BuildCourseResult(
      result_persistence::ModernCourseResult(saved), cancelled, currentPaths,
      [&](const std::filesystem::path &path,
          std::atomic_bool &) -> std::unique_ptr<bms_parser::Chart> {
        requestedPaths.push_back(path);
        const std::size_t index = requestedPaths.size() - 1;
        return parsedChartFor(saved.stages[index].score);
      });
  expect(outcome.value && requestedPaths == currentPaths,
         "moved course recall loads current Records paths in stage order and "
         "validates each against saved identity facts");

  requestedPaths.clear();
  const std::span<const std::filesystem::path> incompletePaths(currentPaths.data(),
                                                                1);
  const auto incomplete = result_recall::BuildCourseResult(
      result_persistence::ModernCourseResult(saved), cancelled,
      incompletePaths,
      [&](const std::filesystem::path &path,
          std::atomic_bool &) -> std::unique_ptr<bms_parser::Chart> {
        requestedPaths.push_back(path);
        return parsedChartFor(saved.stages.front().score);
      });
  expect(!incomplete.value && requestedPaths.empty(),
         "course recall rejects an incomplete current-location projection "
         "before loading any stage");
}

} // namespace

int main() {
  testChartRecallUsesOnlySavedFacts();
  testChartRecallReappliesSavedRandomAndLongNoteSetup();
  testChartRecallFailsClosedBeforePublishing();
  testMovedChartRecallUsesCurrentLocationAndSavedIdentity();
  testCourseRecallIsOrderedCompleteAndAtomic();
  testCourseRecallReappliesEachStageSetupBeforeAgreement();
  testMovedCourseRecallUsesCurrentOrderedLocations();
  if (failures != 0) {
    std::cerr << failures << " modern result recall test(s) failed\n";
    return 1;
  }
  std::cout << "modern result recall tests passed\n";
  return 0;
}
