#include "ModernResult.h"
#include "replay/ReplayFileLifecycle.h"
#include "replay/ReplayPlayback.h"

#include <bit>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

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
  score.finalGauge = 82.5F;
  score.clearType = kClearTypeNormalClearRank;
  score.provenance = ScoreProvenance::Legacy();
  return score;
}

result_persistence::ModernChartResult validChartResult() {
  result_persistence::ModernChartResult result;
  result.attemptId = std::string(kAttemptId);
  result.score = validScore();
  result.keyMode = 7;
  result.adoptedGaugeType = GaugeType::Normal;
  result.adoptedGaugeHistory = {20.0F, 48.5F, 82.5F};
  result.playedAtUnixMillis = 1'700'000'000'123LL;
  result.resultFingerprint =
      result_persistence::modernResultFingerprint(result);
  return result;
}

template <typename Mutator>
void expectChartFingerprintChange(Mutator mutate, std::string_view field) {
  const auto baseline = validChartResult();
  auto changed = baseline;
  mutate(changed);
  changed.resultFingerprint.clear();
  expect(result_persistence::modernResultFingerprint(changed) !=
             baseline.resultFingerprint,
         std::string("chart fingerprint covers ") + std::string(field));
}

void testChartResultIsReplayIndependentAndFullyFingerprinted() {
  const auto result = validChartResult();
  std::string diagnostic;
  expect(result_persistence::validateModernChartResult(result, diagnostic),
         "valid replay-free chart result is accepted");

  replay::ReplayPlaybackData playback;
  playback.setup.chart.sha256 = repeated('f', 64);
  playback.input.emplace_back();
  playback.touchSamples.emplace_back();
  playback.laneCoverEvents.emplace_back();
  replay::ReplayFileMetadata file{
      .relativePath = "replay/ignored.brd",
      .sha256 = repeated('e', 64),
      .compressedSize = 12,
      .codecVersion = 3,
  };
  const std::string before =
      result_persistence::modernResultFingerprint(result);
  playback.setup.chart.sha256[0] = 'd';
  playback.input.front().pressed = true;
  playback.touchSamples.front().x = 0.75F;
  playback.laneCoverEvents.front().noteStartPositionPercent = 91;
  file.sha256[0] = 'c';
  expect(result_persistence::modernResultFingerprint(result) == before,
         "raw replay and file mutations cannot affect result integrity");

  expectChartFingerprintChange([](auto &v) { v.attemptId.back() = '1'; },
                               "attempt identity");
  expectChartFingerprintChange([](auto &v) { v.score.chartPath += "x"; },
                               "chart path");
  expectChartFingerprintChange([](auto &v) { v.score.chartMd5[0] = 'c'; },
                               "chart MD5");
  expectChartFingerprintChange([](auto &v) { v.score.chartSha256[0] = 'c'; },
                               "chart SHA-256");
  expectChartFingerprintChange([](auto &v) { v.score.chartTitle += "x"; },
                               "chart title");
  expectChartFingerprintChange([](auto &v) { ++v.score.good; }, "score facts");
  expectChartFingerprintChange([](auto &v) { v.keyMode = 14; }, "key mode");
  expectChartFingerprintChange(
      [](auto &v) { v.adoptedGaugeType = GaugeType::Hard; },
      "adopted gauge type");
  expectChartFingerprintChange(
      [](auto &v) { v.adoptedGaugeHistory.push_back(1.0F); }, "gauge history");
  expectChartFingerprintChange(
      [](auto &v) {
        v.judgementTiming = result_persistence::ChartJudgementTiming{};
      },
      "judgement timing");
  expectChartFingerprintChange([](auto &v) { ++v.playedAtUnixMillis; },
                               "completion time");
  expectChartFingerprintChange(
      [](auto &v) { v.score.provenance.clubMode = true; }, "provenance");

  auto databaseAssigned = result;
  databaseAssigned.resultId = 99;
  expect(result_persistence::modernResultFingerprint(databaseAssigned) ==
             before,
         "database identity is outside the content fingerprint");

  auto positive = result;
  positive.adoptedGaugeHistory = {0.0F};
  auto negative = positive;
  negative.adoptedGaugeHistory = {-0.0F};
  expect(
      std::bit_cast<std::uint32_t>(positive.adoptedGaugeHistory[0]) !=
              std::bit_cast<std::uint32_t>(negative.adoptedGaugeHistory[0]) &&
          result_persistence::modernResultFingerprint(positive) !=
              result_persistence::modernResultFingerprint(negative),
      "fingerprint preserves signed-zero float bits");
}

void testChartValidationAndFactAgreement() {
  std::string diagnostic;
  auto invalid = validChartResult();
  invalid.attemptId = "not-a-uuid";
  invalid.resultFingerprint.clear();
  expect(!result_persistence::validateModernChartResult(invalid, diagnostic),
         "modern attempt identity is mandatory and canonical");

  invalid = validChartResult();
  invalid.score.chartSha256[0] = 'A';
  invalid.resultFingerprint.clear();
  expect(!result_persistence::validateModernChartResult(invalid, diagnostic),
         "modern chart identity is canonical lowercase");

  invalid = validChartResult();
  invalid.adoptedGaugeHistory.push_back(std::numeric_limits<float>::infinity());
  invalid.resultFingerprint.clear();
  expect(!result_persistence::validateModernChartResult(invalid, diagnostic),
         "non-finite gauge history is rejected");

  invalid = validChartResult();
  invalid.resultFingerprint[0] =
      invalid.resultFingerprint[0] == '0' ? '1' : '0';
  expect(!result_persistence::validateModernChartResult(invalid, diagnostic),
         "stale or foreign result fingerprint is rejected");

  invalid = validChartResult();
  invalid.resultFingerprint =
      "c8744f5007aa619288309622462545732f2ae4a40a772ce5ff012d2542c7dac4";
  expect(!result_persistence::validateModernChartResult(invalid, diagnostic),
         "legacy replay-inclusive fingerprint is rejected");

  invalid = validChartResult();
  invalid.score.pGreat = std::numeric_limits<int>::max();
  invalid.resultFingerprint.clear();
  expect(
      !result_persistence::validateModernChartResult(invalid, diagnostic),
      "score arithmetic rejects oversized judgement totals without overflow");

  auto expected = validChartResult();
  auto materialized = expected;
  materialized.attemptId = "123e4567-e89b-42d3-a456-426614174001";
  ++materialized.playedAtUnixMillis;
  materialized.resultId = 88;
  materialized.score.chartPath = "moved/song.bms";
  materialized.score.chartTitle = "Retitled";
  materialized.score.chartArtist = "Renamed";
  expect(
      result_persistence::compareModernChartResultFacts(expected, materialized)
          .agrees(),
      "agreement ignores ownership, time, and display-only metadata");

  materialized = expected;
  ++materialized.score.score;
  expect(
      result_persistence::compareModernChartResultFacts(expected, materialized)
              .issue == result_persistence::ResultFactAgreementIssue::Score,
      "one comparison authority diagnoses result disagreement");
  materialized = expected;
  materialized.score.provenance.clubMode = true;
  expect(
      result_persistence::compareModernChartResultFacts(expected, materialized)
              .issue ==
          result_persistence::ResultFactAgreementIssue::Provenance,
      "one comparison authority diagnoses provenance disagreement");
}

result_persistence::ModernCourseResult validCourseResult() {
  result_persistence::ModernCourseResult result;
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
  result.maxScore = 30;
  result.maxCombo = 8;
  result.finalGauge = 62.5F;
  result.clearType = kClearTypeHardClearRank;
  result.provenance = ScoreProvenance::Legacy();
  auto second = validScore('d');
  second.maxCombo = 8;
  second.finalGauge = 62.5F;
  result.stages = {
      {.stageIndex = 0,
       .score = validScore('a'),
       .keyMode = 7,
       .adoptedGaugeType = GaugeType::Normal,
       .adoptedGaugeHistory = {20.0F, 70.0F}},
      {.stageIndex = 1,
       .score = std::move(second),
       .keyMode = 14,
       .adoptedGaugeType = GaugeType::Hard,
       .adoptedGaugeHistory = {70.0F, 62.5F}},
  };
  result.entryFacts = {{.totalNotes = 5, .playLengthMicros = 1'000'000},
                       {.totalNotes = 5, .playLengthMicros = 2'000'000},
                       {.totalNotes = 5, .playLengthMicros = 3'000'000}};
  result.playedAtUnixMillis = 1'700'000'000'456LL;
  result.resultFingerprint =
      result_persistence::modernResultFingerprint(result);
  return result;
}

void testCourseResultPrefixAndAggregateContracts() {
  std::string diagnostic;
  const auto result = validCourseResult();
  expect(result_persistence::validateModernCourseResult(result, diagnostic),
         "valid partial modern course result is accepted");

  auto invalid = result;
  invalid.stages[1].stageIndex = 0;
  invalid.resultFingerprint.clear();
  expect(!result_persistence::validateModernCourseResult(invalid, diagnostic),
         "course stage prefix must be contiguous");
  invalid = result;
  invalid.completedCharts = 3;
  invalid.resultFingerprint.clear();
  expect(!result_persistence::validateModernCourseResult(invalid, diagnostic),
         "completed count must equal the saved stage prefix");
  invalid = result;
  ++invalid.finalScore;
  invalid.resultFingerprint.clear();
  expect(!result_persistence::validateModernCourseResult(invalid, diagnostic),
         "course aggregate score must equal ordered stage facts");
  invalid = result;
  invalid.stages[1].score.maxCombo = 3;
  invalid.resultFingerprint.clear();
  expect(!result_persistence::validateModernCourseResult(invalid, diagnostic),
         "carried course maximum combo cannot decrease");
  invalid = result;
  invalid.resultFingerprint = repeated('f', 64);
  expect(!result_persistence::validateModernCourseResult(invalid, diagnostic),
         "legacy or foreign fingerprint cannot authenticate modern course");
  invalid = result;
  invalid.entryFacts[0].totalNotes = std::numeric_limits<int>::max() / 2 + 1;
  invalid.resultFingerprint.clear();
  expect(!result_persistence::validateModernCourseResult(invalid, diagnostic),
         "course score arithmetic rejects overflowing entry facts");

  auto changed = result;
  changed.constraintJson += " ";
  changed.resultFingerprint.clear();
  expect(result_persistence::modernResultFingerprint(changed) !=
             result.resultFingerprint,
         "course fingerprint covers constraints");
  changed = result;
  std::swap(changed.stages[0], changed.stages[1]);
  changed.resultFingerprint.clear();
  expect(result_persistence::modernResultFingerprint(changed) !=
             result.resultFingerprint,
         "course fingerprint covers stage order");

  auto materialized = result;
  materialized.resultId = 41;
  materialized.attemptId = "123e4567-e89b-42d3-a456-426614174001";
  ++materialized.playedAtUnixMillis;
  materialized.courseName = "Renamed";
  materialized.courseGroupName = "Moved";
  materialized.stages[0].score.chartPath = "moved/song.bms";
  materialized.stages[0].score.chartTitle = "Retitled";
  expect(
      result_persistence::compareModernCourseResultFacts(result, materialized)
          .agrees(),
      "course agreement ignores ownership, time, and display metadata");

  materialized = result;
  materialized.initialGaugeType = GaugeType::Easy;
  expect(
      result_persistence::compareModernCourseResultFacts(result, materialized)
              .issue == result_persistence::ResultFactAgreementIssue::Setup,
      "course agreement diagnoses shared setup disagreement");

  materialized = result;
  materialized.stages[1].score.chartSha256[0] = 'e';
  expect(
      result_persistence::compareModernCourseResultFacts(result, materialized)
              .issue ==
          result_persistence::ResultFactAgreementIssue::ChartIdentity,
      "course agreement diagnoses an ordered stage identity disagreement");
}

} // namespace

int main() {
  testChartResultIsReplayIndependentAndFullyFingerprinted();
  testChartValidationAndFactAgreement();
  testCourseResultPrefixAndAggregateContracts();
  if (failures != 0) {
    std::cerr << failures << " modern result test(s) failed\n";
    return 1;
  }
  std::cout << "modern result tests passed\n";
  return 0;
}
