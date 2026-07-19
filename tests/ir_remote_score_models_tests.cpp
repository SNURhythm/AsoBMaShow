#include "ir/IrRemoteScoreModels.h"

#include "scene/play/GameplayGaugeTypes.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

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

ir::IrRemoteScore validScore() {
  return {
      .remoteUserId = 42,
      .game = "bms-7k",
      .remoteScoreId = "score-123",
      .remoteChartId = "chart-456",
      .chartMd5 = repeated('a', 32),
      .chartSha256 = repeated('b', 64),
      .title = "Provider-neutral title",
      .artist = "Provider-neutral artist",
      .service = "client-service",
      .difficulty = "ANOTHER",
      .level = "12",
      .levelNumber = 12.7,
      .noteCount = 1'000,
      .score = 1'800,
      .lampRank = kClearTypeNormalClearRank,
      .timeAchievedUnixMillis = 1'699'999'000'123LL,
      .timeAddedUnixMillis = 1'700'000'000'123LL,
      .judgements =
          {.pGreat = 850, .great = 100, .good = 25, .bad = 15, .poor = 10},
      .timing = {.earlyPGreat = 400,
                 .latePGreat = 450,
                 .earlyGreat = 40,
                 .lateGreat = 60,
                 .earlyGood = 10,
                 .lateGood = 15,
                 .earlyBad = 5,
                 .lateBad = 10,
                 .earlyPoor = 4,
                 .latePoor = 6},
      .fast = 459,
      .slow = 541,
      .maxCombo = 800,
      .badPoints = 25,
      .finalGauge = 82.5F,
      .gaugeHistory = {20.0F, std::nullopt, 0.0F, 100.0F, 82.5F},
      .random = "RANDOM",
      .gauge = "NORMAL",
      .inputDevice = "KEYBOARD",
      .client = "AsoBMaShow",
  };
}

bool valid(const ir::IrRemoteScore &score, std::string &diagnostic) {
  const bool result = ir::validateIrRemoteScore(score, diagnostic);
  expect(diagnostic.size() <= ir::kMaximumIrRemoteScoreDiagnosticBytes,
         "score validation diagnostics are bounded");
  return result;
}

void expectInvalid(ir::IrRemoteScore score, std::string_view message) {
  std::string diagnostic;
  expect(!valid(score, diagnostic), message);
  expect(!diagnostic.empty(), "invalid score has a diagnostic");
}

void expectValid(ir::IrRemoteScore score, std::string_view message) {
  std::string diagnostic = "stale diagnostic";
  expect(valid(score, diagnostic), message);
  expect(diagnostic.empty(), "valid score clears its diagnostic");
}

void testValidScoresAndNullableValues() {
  auto score = validScore();
  expectValid(score, "fully populated 7-key score is valid");

  score.game = "bms-14k";
  score.difficulty.reset();
  score.level.reset();
  score.levelNumber.reset();
  score.timeAchievedUnixMillis.reset();
  score.judgements = {};
  score.timing = {};
  score.fast.reset();
  score.slow.reset();
  score.maxCombo.reset();
  score.badPoints.reset();
  score.finalGauge.reset();
  score.gaugeHistory.clear();
  score.random.reset();
  score.gauge.reset();
  score.inputDevice.reset();
  score.client.reset();
  expectValid(score, "nullable fields may all be absent on a 14-key score");
  expect(!score.judgements.pGreat.has_value(),
         "missing judgement remains absent after validation");

  score.judgements.pGreat = 0;
  expectValid(score, "an explicit zero judgement remains valid");
  expect(score.judgements.pGreat.has_value() && *score.judgements.pGreat == 0,
         "an explicit zero judgement remains present after validation");
  expect(!score.judgements.complete(),
         "one explicit judgement does not make the set complete");

  score.judgements.great = 0;
  score.judgements.good = 0;
  score.judgements.bad = 0;
  score.judgements.poor = 0;
  expect(score.judgements.complete(),
         "all present zero judgements make the set complete");
  expectValid(score, "complete explicit-zero judgements are valid");
}

void testIdentityAndStringBounds() {
  auto score = validScore();
  score.remoteScoreId = repeated('s', ir::kMaximumIrRemoteScoreIdBytes);
  score.remoteChartId = repeated('c', ir::kMaximumIrRemoteScoreIdBytes);
  score.title = repeated('t', ir::kMaximumIrRemoteScoreTextBytes);
  score.artist = repeated('a', ir::kMaximumIrRemoteScoreTextBytes);
  score.service = repeated('v', ir::kMaximumIrRemoteScoreTextBytes);
  score.difficulty = repeated('d', ir::kMaximumIrRemoteScoreTextBytes);
  score.level = repeated('l', ir::kMaximumIrRemoteScoreTextBytes);
  score.random = repeated('r', ir::kMaximumIrRemoteScoreTextBytes);
  score.gauge = repeated('g', ir::kMaximumIrRemoteScoreTextBytes);
  score.inputDevice = repeated('i', ir::kMaximumIrRemoteScoreTextBytes);
  score.client = repeated('c', ir::kMaximumIrRemoteScoreTextBytes);
  expectValid(score, "remote IDs and text at their bounds are valid");

  score = validScore();
  score.game = "bms-5k";
  expectInvalid(score, "unsupported games are rejected");

  score = validScore();
  score.remoteUserId = 0;
  expectInvalid(score, "zero remote user ID is rejected");

  score = validScore();
  score.remoteUserId = -1;
  expectInvalid(score, "negative remote user ID is rejected");

  score = validScore();
  score.remoteScoreId.clear();
  expectInvalid(score, "empty remote score ID is rejected");

  score = validScore();
  score.remoteChartId.clear();
  expectInvalid(score, "empty remote chart ID is rejected");

  score = validScore();
  score.remoteScoreId = repeated('s', ir::kMaximumIrRemoteScoreIdBytes + 1);
  expectInvalid(score, "oversized remote score ID is rejected");

  score = validScore();
  score.remoteChartId = "chart\nprivate-token";
  expectInvalid(score, "control characters in remote IDs are rejected");

  score = validScore();
  score.title = repeated('t', ir::kMaximumIrRemoteScoreTextBytes + 1);
  expectInvalid(score, "oversized required text is rejected");

  score = validScore();
  score.difficulty = "ANOTHER\x7fhidden";
  expectInvalid(score, "control characters in optional text are rejected");

  score = validScore();
  score.client = repeated('x', ir::kMaximumIrRemoteScoreTextBytes + 1);
  expectInvalid(score, "oversized optional text is rejected");

  score = validScore();
  score.remoteScoreId = "score\ncredential-secret";
  std::string diagnostic;
  expect(!valid(score, diagnostic), "secret-bearing invalid score is rejected");
  expect(diagnostic.find("credential-secret") == std::string::npos,
         "diagnostic never serializes remote score contents");
}

void testHashesAndNumericBounds() {
  auto score = validScore();
  score.chartMd5 = repeated('A', 32);
  expectInvalid(score, "upper-case MD5 is rejected");

  score = validScore();
  score.chartMd5 = repeated('a', 31);
  expectInvalid(score, "wrong-length MD5 is rejected");

  score = validScore();
  score.chartSha256 = repeated('g', 64);
  expectInvalid(score, "non-hex SHA-256 is rejected");

  score = validScore();
  score.chartSha256.clear();
  expectValid(score, "canonical MD5 alone is a valid fallback identity");

  score = validScore();
  score.chartMd5.clear();
  expectValid(score, "canonical SHA-256 alone is a valid chart identity");

  score = validScore();
  score.chartMd5.clear();
  score.chartSha256.clear();
  expectInvalid(score, "missing MD5 and SHA-256 are rejected");

  score = validScore();
  score.levelNumber = std::numeric_limits<double>::quiet_NaN();
  expectInvalid(score, "NaN level number is rejected");

  score = validScore();
  score.levelNumber = std::numeric_limits<double>::infinity();
  expectInvalid(score, "infinite level number is rejected");

  score = validScore();
  score.levelNumber = -0.1;
  expectInvalid(score, "negative level number is rejected");

  score = validScore();
  score.noteCount = -1;
  expectInvalid(score, "negative note count is rejected");

  score = validScore();
  score.score = -1;
  expectInvalid(score, "negative EX score is rejected");

  score = validScore();
  score.noteCount = std::numeric_limits<int>::max();
  score.score = 0;
  expectInvalid(score, "overflowing maximum EX score is rejected");

  score = validScore();
  score.noteCount = 10;
  score.score = 21;
  expectInvalid(score, "EX score above twice the note count is rejected");

  score = validScore();
  score.lampRank = 999;
  expectInvalid(score, "unknown lamp rank is rejected");

  score = validScore();
  score.timeAchievedUnixMillis = 0;
  expectInvalid(score, "present non-positive achieved time is rejected");

  score = validScore();
  score.timeAddedUnixMillis = 0;
  expectInvalid(score, "non-positive added time is rejected");
}

void testOptionalMetricBounds() {
  auto score = validScore();
  score.judgements.good = -1;
  expectInvalid(score, "negative judgement count is rejected");

  score = validScore();
  score.timing.lateBad = -1;
  expectInvalid(score, "negative timing count is rejected");

  score = validScore();
  score.fast = -1;
  expectInvalid(score, "negative fast count is rejected");

  score = validScore();
  score.slow = -1;
  expectInvalid(score, "negative slow count is rejected");

  score = validScore();
  score.maxCombo = -1;
  expectInvalid(score, "negative maximum combo is rejected");

  score = validScore();
  score.maxCombo = score.noteCount + 1;
  expectInvalid(score, "maximum combo above note count is rejected");

  score = validScore();
  score.badPoints = -1;
  expectInvalid(score, "negative bad points is rejected");
}

void testGaugeBounds() {
  auto score = validScore();
  score.gaugeHistory.assign(ir::kMaximumIrRemoteGaugeHistoryEntries,
                            std::nullopt);
  expectValid(score, "gauge history at the collection bound is valid");

  score.gaugeHistory.push_back(std::nullopt);
  expectInvalid(score, "oversized gauge history is rejected");

  score = validScore();
  score.finalGauge = std::numeric_limits<float>::quiet_NaN();
  expectInvalid(score, "NaN final gauge is rejected");

  score = validScore();
  score.finalGauge = -0.01F;
  expectInvalid(score, "negative final gauge is rejected");

  score = validScore();
  score.finalGauge = 100.01F;
  expectInvalid(score, "final gauge above 100 is rejected");

  score = validScore();
  score.gaugeHistory = {std::numeric_limits<float>::infinity()};
  expectInvalid(score, "infinite gauge history value is rejected");

  score = validScore();
  score.gaugeHistory = {std::nullopt, 101.0F};
  expectInvalid(score, "gauge history value above 100 is rejected");
}

void testSnapshotBoundsAndDuplicateIdentity() {
  std::string diagnostic = "stale diagnostic";
  ir::IrUserScoreSnapshot empty;
  expect(ir::validateIrUserScoreSnapshot(empty, diagnostic),
         "empty complete snapshot is valid");
  expect(diagnostic.empty(), "valid snapshot clears its diagnostic");

  auto first = validScore();
  auto second = validScore();
  second.game = "bms-14k";
  second.remoteChartId = "another-chart";
  expect(
      !ir::validateIrUserScoreSnapshot({.scores = {first, second}}, diagnostic),
      "duplicate remote score IDs within a snapshot are rejected");
  expect(!diagnostic.empty(), "invalid snapshot has a diagnostic");
  expect(diagnostic.size() <= ir::kMaximumIrRemoteScoreDiagnosticBytes,
         "snapshot validation diagnostics are bounded");
  expect(diagnostic.find(first.remoteScoreId) == std::string::npos,
         "duplicate diagnostic does not serialize the remote score ID");

  ir::IrUserScoreSnapshot oversized;
  oversized.scores.resize(ir::kMaximumIrRemoteScoreSnapshotEntries + 1);
  expect(!ir::validateIrUserScoreSnapshot(oversized, diagnostic),
         "oversized score snapshot is rejected before row validation");
  expect(diagnostic.size() <= ir::kMaximumIrRemoteScoreDiagnosticBytes,
         "oversized snapshot diagnostic is bounded");

  ir::IrUserScoreSnapshot invalidRow{.scores = {validScore()}};
  invalidRow.scores.front().score = -1;
  expect(!ir::validateIrUserScoreSnapshot(invalidRow, diagnostic),
         "snapshot rejects an invalid score row");
}

void testOutcomeDefaultsAreFailureSafe() {
  const ir::IrUserScoreSnapshotOutcome outcome;
  expect(outcome.status == ir::IrUserScoreSnapshotStatus::MalformedResponse,
         "snapshot outcome defaults to malformed response");
  expect(!outcome.snapshot.has_value(),
         "default snapshot outcome carries no snapshot");
  expect(outcome.code.empty() && outcome.diagnostic.empty(),
         "default snapshot outcome carries no unbounded detail");
}

} // namespace

int main() {
  testValidScoresAndNullableValues();
  testIdentityAndStringBounds();
  testHashesAndNumericBounds();
  testOptionalMetricBounds();
  testGaugeBounds();
  testSnapshotBoundsAndDuplicateIdentity();
  testOutcomeDefaultsAreFailureSafe();
  if (failures != 0) {
    std::cerr << failures << " IR remote score model assertion(s) failed\n";
    return 1;
  }
  std::cout << "IR remote score model tests passed\n";
  return 0;
}
