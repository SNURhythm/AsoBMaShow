#include "ResultPersistenceModel.h"
#include "replay/ReplayPlaybackData.h"

#include <bit>
#include <iostream>
#include <limits>
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

result_persistence::ChartScoreWrite validScore(char hash = 'a') {
  result_persistence::ChartScoreWrite score;
  score.chartPath = "sample/song.bms";
  score.chartMd5 = repeated(hash == 'a' ? 'b' : hash, 32);
  score.chartSha256 = repeated(hash, 64);
  score.chartTitle = "Title";
  score.chartArtist = "Artist";
  score.longNoteMode = 1;
  score.score = 7;
  score.maxScore = 10;
  score.maxCombo = 4;
  score.comboBreak = 1;
  score.pGreat = 3;
  score.great = 1;
  score.good = 1;
  score.fast = 0;
  score.slow = 0;
  score.finalGauge = 82.5F;
  score.clearType = kClearTypeNormalClearRank;
  score.provenance = ScoreProvenance::Legacy();
  return score;
}

ScoreProvenance provenanceFor(const result_persistence::ChartScoreWrite &score,
                              replay::DoublePlayOption doublePlayOption,
                              GaugeType gaugeType) {
  ScoreProvenanceBuildInput input;
  input.chartMeta.MD5 = score.chartMd5;
  input.chartMeta.SHA256 = score.chartSha256;
  input.chartMeta.Rank = 1;
  input.chartMeta.TotalNotes = score.maxScore / 2;
  input.chartMeta.HasTotal = true;
  input.chartMeta.Total = 200.0;
  input.longNoteMode = score.longNoteMode;
  input.effectiveJudgeWindows = {
      {PGreat, {-10'000, 10'000}}, {Great, {-30'000, 30'000}},
      {Good, {-75'000, 75'000}},   {Bad, {-330'000, 420'000}},
      {Kpoor, {-500'000, 150'000}},
  };
  input.totalNotes = input.chartMeta.TotalNotes;
  input.authoredGaugeTotal = input.chartMeta.Total;
  input.effectiveGaugeTotal = input.chartMeta.Total;
  input.gaugeType = gaugeType;
  input.doublePlayOption = doublePlayOption;
  return makeScoreProvenance(input);
}

result_persistence::PersistedChartResult validResult() {
  result_persistence::PersistedChartResult result;
  result.attemptId = std::string(kAttemptId);
  result.score = validScore();
  result.keyMode = 7;
  result.adoptedGaugeType = GaugeType::Hard;
  result.adoptedGaugeHistory = {20.0F, 48.5F, 82.5F};
  result.playedAtUnixMillis = 1'700'000'000'123LL;
  result.resultFingerprint = result_persistence::resultFingerprint(result);
  return result;
}

result_persistence::PersistedCourseResult validCourse();

template <typename Mutator>
void expectChartFingerprintChange(Mutator mutate, std::string_view field) {
  const auto baseline = validResult();
  auto changed = baseline;
  mutate(changed);
  changed.resultFingerprint.clear();
  expect(result_persistence::resultFingerprint(changed) !=
             baseline.resultFingerprint,
         std::string("chart fingerprint covers ") + std::string(field));
}

void testChartResultIndependenceAndFingerprint() {
  auto result = validResult();
  std::string diagnostic;
  expect(result_persistence::validatePersistedChartResult(result, diagnostic),
         "valid independent chart result is accepted");
  expect(diagnostic.empty(), "valid result clears diagnostic");

  replay::ReplayPlaybackData replay;
  replay.input.push_back({.songTimeMicros = 100,
                          .control = {.kind = replay::LogicalControlKind::Lane,
                                      .player = 1,
                                      .lane = 2},
                          .pressed = true});
  replay.touchSamples.push_back({.fingerId = 4, .x = 0.2F, .y = 0.8F});
  replay.laneCoverEvents.push_back(
      {.songTimeMicros = 200, .noteStartPositionPercent = 31});
  replay.legacy = replay::LegacyPlaybackTrack{};
  const std::string before = result_persistence::resultFingerprint(result);
  replay.setup.chartSha256 = repeated('f', 64);
  replay.input.front().pressed = false;
  replay.touchSamples.front().x = 0.9F;
  replay.laneCoverEvents.front().noteStartPositionPercent = 80;
  replay.legacy->events.emplace_back();
  expect(result_persistence::resultFingerprint(result) == before,
         "result fingerprint is independent from all replay domains");

  expectChartFingerprintChange(
      [](auto &value) {
        value.attemptId = "123e4567-e89b-42d3-a456-426614174001";
      },
      "attemptId");
  expectChartFingerprintChange(
      [](auto &value) { value.score.chartPath += "x"; }, "chartPath");
  expectChartFingerprintChange(
      [](auto &value) { value.score.chartMd5[0] = 'c'; }, "chartMd5");
  expectChartFingerprintChange(
      [](auto &value) { value.score.chartSha256[0] = 'c'; }, "chartSha256");
  expectChartFingerprintChange(
      [](auto &value) { value.score.chartTitle += "x"; }, "chartTitle");
  expectChartFingerprintChange(
      [](auto &value) { value.score.chartArtist += "x"; }, "chartArtist");
  expectChartFingerprintChange([](auto &value) { ++value.score.longNoteMode; },
                               "longNoteMode");
  expectChartFingerprintChange([](auto &value) { ++value.score.good; },
                               "judgement totals");
  expectChartFingerprintChange([](auto &value) { value.keyMode = 14; },
                               "keyMode");
  expectChartFingerprintChange(
      [](auto &value) { value.adoptedGaugeType = GaugeType::Easy; },
      "adoptedGaugeType");
  expectChartFingerprintChange(
      [](auto &value) { value.adoptedGaugeHistory.push_back(1.0F); },
      "gaugeHistory");
  expectChartFingerprintChange(
      [](auto &value) {
        value.judgementTiming = result_persistence::ChartJudgementTiming{};
      },
      "judgementTiming");
  expectChartFingerprintChange([](auto &value) { ++value.playedAtUnixMillis; },
                               "playedAtUnixMillis");
  expectChartFingerprintChange(
      [](auto &value) { value.score.provenance.autoPlay = true; },
      "provenance");

  auto databaseAssigned = result;
  databaseAssigned.resultId = 99;
  expect(result_persistence::resultFingerprint(databaseAssigned) == before,
         "database result ID is outside the content fingerprint");

  auto positiveZero = result;
  positiveZero.adoptedGaugeHistory = {0.0F};
  auto negativeZero = positiveZero;
  negativeZero.adoptedGaugeHistory = {-0.0F};
  expect(result_persistence::resultFingerprint(positiveZero) !=
             result_persistence::resultFingerprint(negativeZero),
         "canonical fingerprint preserves float bit patterns");
}

void testDoublePlaySetupFingerprintContract() {
  auto normal = validResult();
  normal.score.provenance = provenanceFor(
      normal.score, replay::DoublePlayOption::Normal, normal.adoptedGaugeType);
  normal.resultFingerprint = result_persistence::resultFingerprint(normal);

  auto flip = normal;
  flip.score.provenance.stages.front().doublePlayOption =
      replay::DoublePlayOption::Flip;
  flip.resultFingerprint.clear();
  expect(result_persistence::resultFingerprint(flip) !=
             normal.resultFingerprint,
         "chart fingerprint binds the double-play option");

  auto normalCourse = validCourse();
  std::vector<ScoreProvenance> stageProvenance;
  for (auto &stage : normalCourse.stages) {
    stage.score.provenance = provenanceFor(
        stage.score, replay::DoublePlayOption::Normal, stage.adoptedGaugeType);
    stageProvenance.push_back(stage.score.provenance);
  }
  normalCourse.provenance = mergeCourseProvenance(stageProvenance);
  normalCourse.resultFingerprint =
      result_persistence::resultFingerprint(normalCourse);

  auto flipCourse = normalCourse;
  flipCourse.stages[1].score.provenance.stages.front().doublePlayOption =
      replay::DoublePlayOption::Flip;
  flipCourse.provenance.stages[1].doublePlayOption =
      replay::DoublePlayOption::Flip;
  flipCourse.resultFingerprint.clear();
  expect(result_persistence::resultFingerprint(flipCourse) !=
             normalCourse.resultFingerprint,
         "course fingerprint binds each stage double-play option");

  auto inconsistentCourse = normalCourse;
  inconsistentCourse.provenance.stages[1].doublePlayOption =
      replay::DoublePlayOption::Flip;
  inconsistentCourse.resultFingerprint =
      result_persistence::resultFingerprint(inconsistentCourse);
  std::string diagnostic;
  expect(!result_persistence::validatePersistedCourseResult(inconsistentCourse,
                                                            diagnostic),
         "course aggregate provenance must equal its ordered stage proofs");

  auto schemaFourNormal = normal;
  schemaFourNormal.score.provenance.schemaVersion = 4;
  schemaFourNormal.score.provenance.stages.front().doublePlayOption =
      replay::DoublePlayOption::Normal;
  auto schemaFourFlip = schemaFourNormal;
  schemaFourFlip.score.provenance.stages.front().doublePlayOption =
      replay::DoublePlayOption::Flip;
  expect(result_persistence::resultFingerprint(schemaFourNormal) ==
             result_persistence::resultFingerprint(schemaFourFlip),
         "schema-four fingerprints remain independent of unavailable DP data");

  std::vector<ScoreProvenance> schemaFourStages = stageProvenance;
  for (auto &provenance : schemaFourStages) {
    provenance.schemaVersion = ScoreProvenance::kPolicyProofSchemaVersion;
    for (auto &stage : provenance.stages) {
      stage.doublePlayOption.reset();
    }
  }
  const ScoreProvenance mergedSchemaFour =
      mergeCourseProvenance(schemaFourStages);
  std::string serializationError;
  expect(
      mergedSchemaFour.schemaVersion ==
              ScoreProvenance::kPolicyProofSchemaVersion &&
          serializeValidatedScoreProvenance(mergedSchemaFour,
                                            serializationError)
              .has_value(),
      "merging schema-four stage proofs remains serializable as schema four");

  auto legacyEligibilityAggregate = normalCourse;
  legacyEligibilityAggregate.provenance.eligibility =
      ScoreEligibility::LegacyUnverified;
  legacyEligibilityAggregate.resultFingerprint =
      result_persistence::resultFingerprint(legacyEligibilityAggregate);
  diagnostic.clear();
  expect(!result_persistence::validatePersistedCourseResult(
             legacyEligibilityAggregate, diagnostic),
         "schema-five aggregate eligibility remains bound to stage proofs");

  auto mixedSchemaCourse = normalCourse;
  mixedSchemaCourse.stages.back().score.provenance.schemaVersion =
      ScoreProvenance::kPolicyProofSchemaVersion;
  mixedSchemaCourse.stages.back()
      .score.provenance.stages.front()
      .doublePlayOption.reset();
  mixedSchemaCourse.resultFingerprint =
      result_persistence::resultFingerprint(mixedSchemaCourse);
  diagnostic.clear();
  expect(!result_persistence::validatePersistedCourseResult(mixedSchemaCourse,
                                                            diagnostic),
         "schema-five aggregate rejects mixed-version ordered stage proofs");

  auto allSchemaFourCourse = normalCourse;
  allSchemaFourCourse.provenance.schemaVersion =
      ScoreProvenance::kPolicyProofSchemaVersion;
  for (auto &stage : allSchemaFourCourse.provenance.stages) {
    stage.doublePlayOption.reset();
  }
  for (auto &stage : allSchemaFourCourse.stages) {
    stage.score.provenance.schemaVersion =
        ScoreProvenance::kPolicyProofSchemaVersion;
    stage.score.provenance.stages.front().doublePlayOption.reset();
  }
  allSchemaFourCourse.resultFingerprint =
      result_persistence::resultFingerprint(allSchemaFourCourse);
  diagnostic.clear();
  expect(result_persistence::validatePersistedCourseResult(allSchemaFourCourse,
                                                           diagnostic),
         "all-schema-four course provenance remains backward compatible");
}

void testChartValidation() {
  std::string diagnostic;
  auto invalid = validResult();
  invalid.attemptId = "NOT-A-UUID";
  invalid.resultFingerprint.clear();
  expect(!result_persistence::validatePersistedChartResult(invalid, diagnostic),
         "noncanonical attempt ID is rejected");

  invalid = validResult();
  invalid.playedAtUnixMillis = -1;
  invalid.resultFingerprint.clear();
  expect(!result_persistence::validatePersistedChartResult(invalid, diagnostic),
         "negative play time is rejected");

  invalid = validResult();
  invalid.score.chartSha256[0] = 'A';
  invalid.resultFingerprint.clear();
  expect(!result_persistence::validatePersistedChartResult(invalid, diagnostic),
         "noncanonical chart identity is rejected");

  invalid = validResult();
  invalid.adoptedGaugeHistory.push_back(std::numeric_limits<float>::infinity());
  invalid.resultFingerprint.clear();
  expect(!result_persistence::validatePersistedChartResult(invalid, diagnostic),
         "nonfinite gauge history is rejected");

  invalid = validResult();
  result_persistence::ChartJudgementTiming timing;
  timing.byJudgement[PGreat] = {.fast = 1, .slow = 0};
  invalid.judgementTiming = timing;
  invalid.resultFingerprint.clear();
  expect(!result_persistence::validatePersistedChartResult(invalid, diagnostic),
         "timing totals must match aggregate timing");

  invalid = validResult();
  invalid.resultFingerprint[0] =
      invalid.resultFingerprint[0] == '0' ? '1' : '0';
  expect(!result_persistence::validatePersistedChartResult(invalid, diagnostic),
         "tampered result fingerprint is rejected");
}

result_persistence::PersistedCourseResult validCourse() {
  result_persistence::PersistedCourseResult result;
  result.attemptId = std::string(kAttemptId);
  result.courseKey = "course:v1:" + repeated('c', 64);
  result.legacyCourseId = 4;
  result.courseName = "Course";
  result.courseGroupName = "Group";
  result.constraintJson = "[\"ln\"]";
  result.completedCharts = 2;
  result.totalCharts = 3;
  result.requestedPlayOption = "RANDOM";
  result.assistOption = "OFF";
  result.initialGaugeType = GaugeType::Hard;
  result.gaugeProfile = GaugeProfile::Standard;
  result.gaugeAutoShift = GaugeAutoShiftMode::None;
  result.gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  result.longNoteMode = 1;
  result.finalScore = 14;
  result.maxScore = 34;
  result.maxCombo = 8;
  result.finalGauge = 62.5F;
  result.clearType = kClearTypeHardClearRank;
  result.provenance = ScoreProvenance::Legacy();
  result.stages = {
      {.stageIndex = 0,
       .score = validScore('a'),
       .keyMode = 7,
       .adoptedGaugeType = GaugeType::Hard,
       .adoptedGaugeHistory = {20.0F, 70.0F}},
      {.stageIndex = 1,
       .score = validScore('d'),
       .keyMode = 14,
       .adoptedGaugeType = GaugeType::Normal,
       .adoptedGaugeHistory = {70.0F, 62.5F}},
  };
  result.entryFacts = {
      {.totalNotes = 5, .playLengthMicros = 1'000'000},
      {.totalNotes = 5, .playLengthMicros = 2'000'000},
      {.totalNotes = 7, .playLengthMicros = 3'000'000},
  };
  result.playedAtUnixMillis = 1'700'000'000'456LL;
  result.resultFingerprint = result_persistence::resultFingerprint(result);
  return result;
}

template <typename Mutator>
void expectCourseFingerprintChange(Mutator mutate, std::string_view field) {
  const auto baseline = validCourse();
  auto changed = baseline;
  mutate(changed);
  changed.resultFingerprint.clear();
  expect(result_persistence::resultFingerprint(changed) !=
             baseline.resultFingerprint,
         std::string("course fingerprint covers ") + std::string(field));
}

void testCourseResult() {
  auto result = validCourse();
  std::string diagnostic;
  expect(result_persistence::validatePersistedCourseResult(result, diagnostic),
         "partial course maximum includes every entry while stage maxima "
         "remain per-stage");

  result.resultId = 42;
  const auto secondStage =
      result_persistence::chartResultForCourseStage(result, 1);
  expect(secondStage.has_value() && secondStage->resultId == 42 &&
             secondStage->attemptId == result.attemptId &&
             secondStage->score == result.stages[1].score &&
             secondStage->keyMode == result.stages[1].keyMode &&
             secondStage->playedAtUnixMillis == result.playedAtUnixMillis,
         "course replay analysis projects one complete stage result from the "
         "saved course context");
  expect(!result_persistence::chartResultForCourseStage(result, 2),
         "course replay analysis rejects a stage beyond completed results");
  expectCourseFingerprintChange(
      [](auto &value) { value.constraintJson += " "; }, "constraints");
  expectCourseFingerprintChange(
      [](auto &value) { value.requestedPlayOption = "MIRROR"; },
      "requested option");
  expectCourseFingerprintChange(
      [](auto &value) { value.initialGaugeType = GaugeType::ExHard; },
      "gauge configuration");
  expectCourseFingerprintChange(
      [](auto &value) { value.provenance.clubMode = true; },
      "course provenance");
  expectCourseFingerprintChange(
      [](auto &value) { std::swap(value.stages[0], value.stages[1]); },
      "stage order");
  expectCourseFingerprintChange(
      [](auto &value) { value.stages[0].adoptedGaugeHistory[0] += 1.0F; },
      "stage facts");
  expectCourseFingerprintChange(
      [](auto &value) { value.stages[0].adoptedGaugeType = GaugeType::Easy; },
      "stage adopted gauge type");
  expectCourseFingerprintChange(
      [](auto &value) { ++value.entryFacts[2].totalNotes; },
      "unfinished stage note count");
  expectCourseFingerprintChange(
      [](auto &value) { ++value.entryFacts[2].playLengthMicros; },
      "unfinished stage play length");

  auto invalid = result;
  invalid.stages[1].stageIndex = 0;
  invalid.resultFingerprint.clear();
  expect(
      !result_persistence::validatePersistedCourseResult(invalid, diagnostic),
      "noncontiguous course stages are rejected");
  invalid = result;
  invalid.completedCharts = 3;
  invalid.resultFingerprint.clear();
  expect(
      !result_persistence::validatePersistedCourseResult(invalid, diagnostic),
      "partial completion count must match stages");
  invalid = result;
  ++invalid.finalScore;
  invalid.resultFingerprint.clear();
  expect(
      !result_persistence::validatePersistedCourseResult(invalid, diagnostic),
      "course totals must equal ordered stage totals");
  invalid = result;
  invalid.entryFacts.pop_back();
  invalid.resultFingerprint.clear();
  expect(
      !result_persistence::validatePersistedCourseResult(invalid, diagnostic),
      "every course entry must retain presentation facts");
  invalid = result;
  invalid.entryFacts[2].playLengthMicros = -1;
  invalid.resultFingerprint.clear();
  expect(
      !result_persistence::validatePersistedCourseResult(invalid, diagnostic),
      "course entry presentation facts must be nonnegative");
  invalid = result;
  invalid.initialGaugeType = static_cast<GaugeType>(99);
  invalid.resultFingerprint.clear();
  expect(
      !result_persistence::validatePersistedCourseResult(invalid, diagnostic),
      "course initial gauge type must be recognized");
  invalid = result;
  invalid.gaugeProfile = static_cast<GaugeProfile>(99);
  invalid.resultFingerprint.clear();
  expect(
      !result_persistence::validatePersistedCourseResult(invalid, diagnostic),
      "course gauge profile must be recognized");
  invalid = result;
  invalid.gaugeAutoShift = static_cast<GaugeAutoShiftMode>(99);
  invalid.resultFingerprint.clear();
  expect(
      !result_persistence::validatePersistedCourseResult(invalid, diagnostic),
      "course gauge auto-shift mode must be recognized");
  invalid = result;
  invalid.gaugeAutoShiftLowerBound = static_cast<GaugeType>(99);
  invalid.resultFingerprint.clear();
  expect(
      !result_persistence::validatePersistedCourseResult(invalid, diagnostic),
      "course gauge auto-shift lower bound must be recognized");
  invalid = result;
  invalid.longNoteMode = 99;
  invalid.resultFingerprint.clear();
  expect(
      !result_persistence::validatePersistedCourseResult(invalid, diagnostic),
      "course long-note mode must be supported");
}

} // namespace

int main() {
  testChartResultIndependenceAndFingerprint();
  testDoublePlaySetupFingerprintContract();
  testChartValidation();
  testCourseResult();
  if (failures != 0) {
    std::cerr << failures << " result persistence model test(s) failed\n";
    return 1;
  }
  std::cout << "result persistence model tests passed\n";
  return 0;
}
