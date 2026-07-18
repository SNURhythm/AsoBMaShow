#include "ir/tachi/TachiBatchManual.h"

#include "nlohmann/json.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
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

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

ir::IrSubmission validSubmission() {
  ir::IrSubmission submission;
  submission.attemptId = "123e4567-e89b-42d3-a456-426614174000";
  submission.keyMode = 7;
  submission.chartMd5 = repeated('b', 32);
  submission.chartSha256 = repeated('a', 64);
  submission.score = 1100;
  submission.maxScore = 1200;
  submission.maxCombo = 550;
  submission.comboBreak = 3;
  submission.pGreat = 500;
  submission.great = 100;
  submission.good = 10;
  submission.bad = 2;
  submission.poor = 1;
  submission.kPoor = 7;
  submission.fast = 30;
  submission.slow = 40;
  submission.finalGauge = 82.0F;
  submission.clearType = kClearTypeHardClearRank;
  submission.playedAtUnixMillis = 1700000000000LL;
  return submission;
}

nlohmann::json builtDocument(const ir::IrSubmission &submission) {
  const auto outcome = ir::tachi::buildBatchManualDraft(submission);
  expect(outcome.status == ir::BuildDraftStatus::Built,
         "valid submission builds a draft");
  expect(outcome.draft.has_value(), "built outcome contains draft");
  if (!outcome.draft.has_value()) {
    return {};
  }
  return nlohmann::json::parse(outcome.draft->payloadJson);
}

void testBuildsOneScoreBatchManual() {
  const auto submission = validSubmission();
  const auto outcome = ir::tachi::buildBatchManualDraft(submission);
  expect(outcome.status == ir::BuildDraftStatus::Built,
         "valid payload status is built");
  expect(outcome.draft.has_value(), "valid payload returns draft");
  if (!outcome.draft.has_value()) {
    return;
  }
  expect(outcome.draft->providerId == ir::tachi::kProviderId,
         "draft uses stable provider ID");
  expect(outcome.draft->attemptId == submission.attemptId,
         "draft retains attempt ID");
  expect(outcome.draft->chartMd5 == submission.chartMd5 &&
             outcome.draft->chartSha256 == submission.chartSha256,
         "draft retains chart hashes");
  expect(outcome.draft->createdAtUnixMillis ==
             submission.playedAtUnixMillis,
         "draft retains captured play time");

  const auto document = nlohmann::json::parse(outcome.draft->payloadJson);
  expect(document.at("meta").at("game") == "bms", "game is BMS");
  expect(document.at("meta").at("playtype") == "7K", "7K playtype maps");
  expect(document.at("meta").at("service") == "AsoBMaShow",
         "service identifies application");
  expect(document.at("scores").size() == 1, "payload contains one score");
  const auto &score = document.at("scores").at(0);
  expect(score.at("score") == 1100, "payload uses EX score");
  expect(score.at("lamp") == "HARD CLEAR", "payload maps hard clear");
  expect(score.at("matchType") == "bmsChartHash",
         "payload uses BMS hash matching");
  expect(score.at("identifier") == repeated('a', 64),
         "payload prefers SHA-256");
  expect(score.at("timeAchieved") == 1700000000000LL,
         "payload uses captured timestamp");
  expect(score.at("judgements").size() == 5,
         "payload emits only Tachi BMS judgements");
  expect(!score.at("judgements").contains("kpoor"),
         "payload omits KPoor");
  expect(score.at("optional").at("bp") == 3,
         "BP is bad plus poor only");
  expect(score.at("optional").at("maxCombo") == 550,
         "payload includes max combo");
  expect(score.at("optional").at("gauge") == 82.0,
         "payload includes final gauge");
}

void testMapsPlaytypesAndHashFallback() {
  auto submission = validSubmission();
  submission.keyMode = 14;
  submission.chartSha256.clear();
  const auto document = builtDocument(submission);
  expect(document.at("meta").at("playtype") == "14K",
         "14K playtype maps");
  expect(document.at("scores").at(0).at("identifier") == repeated('b', 32),
         "MD5 is used when SHA-256 is absent");

  submission.keyMode = 5;
  const auto unsupported = ir::tachi::buildBatchManualDraft(submission);
  expect(unsupported.status == ir::BuildDraftStatus::Unsupported,
         "unsupported key mode is not retryable invalid data");
  expect(!unsupported.draft.has_value(),
         "unsupported key mode creates no draft");
}

void testMapsEveryLamp() {
  constexpr std::array cases{
      std::pair{kClearTypeFailedRank, "FAILED"},
      std::pair{kClearTypeAssistedEasyClearRank, "ASSIST CLEAR"},
      std::pair{kClearTypeLightAssistedEasyClearRank, "ASSIST CLEAR"},
      std::pair{kClearTypeEasyClearRank, "EASY CLEAR"},
      std::pair{kClearTypeNormalClearRank, "CLEAR"},
      std::pair{kClearTypeHardClearRank, "HARD CLEAR"},
      std::pair{kClearTypeExHardClearRank, "EX HARD CLEAR"},
      std::pair{kClearTypeFullComboRank, "FULL COMBO"},
  };
  for (const auto &[rank, expectedLamp] : cases) {
    auto submission = validSubmission();
    submission.clearType = rank;
    const auto document = builtDocument(submission);
    expect(document.at("scores").at(0).at("lamp") == expectedLamp,
           std::string("lamp maps to ") + expectedLamp);
  }
}

void testClampsGauge() {
  auto submission = validSubmission();
  submission.finalGauge = -4.0F;
  auto document = builtDocument(submission);
  expect(document.at("scores").at(0).at("optional").at("gauge") == 0.0,
         "negative gauge clamps to zero");

  submission.finalGauge = 120.0F;
  document = builtDocument(submission);
  expect(document.at("scores").at(0).at("optional").at("gauge") == 100.0,
         "high gauge clamps to one hundred");
}

void testRejectsMalformedSubmission() {
  auto expectInvalid = [](ir::IrSubmission submission,
                          std::string_view message) {
    const auto outcome = ir::tachi::buildBatchManualDraft(submission);
    expect(outcome.status == ir::BuildDraftStatus::Invalid, message);
    expect(!outcome.draft.has_value(), "invalid input creates no draft");
  };

  auto submission = validSubmission();
  submission.chartSha256 = "bad";
  expectInvalid(submission, "malformed SHA-256 is invalid");

  submission = validSubmission();
  submission.chartSha256.clear();
  submission.chartMd5.clear();
  expectInvalid(submission, "missing chart hash is invalid");

  submission = validSubmission();
  submission.score = 1101;
  expectInvalid(submission, "inconsistent EX score is invalid");

  submission = validSubmission();
  submission.poor = -1;
  expectInvalid(submission, "negative judgement is invalid");

  submission = validSubmission();
  submission.playedAtUnixMillis = 0;
  expectInvalid(submission, "nonpositive timestamp is invalid");

  submission = validSubmission();
  submission.finalGauge = std::numeric_limits<float>::quiet_NaN();
  expectInvalid(submission, "non-finite gauge is invalid");

  submission = validSubmission();
  submission.clearType = 12345;
  expectInvalid(submission, "unknown clear rank is invalid");

  submission = validSubmission();
  submission.bad = std::numeric_limits<int>::max();
  submission.poor = 1;
  expectInvalid(submission, "BP integer overflow is invalid");
}

void testPayloadNeverContainsCredentialMaterial() {
  const auto outcome = ir::tachi::buildBatchManualDraft(validSubmission());
  expect(outcome.draft.has_value(), "credential hygiene draft builds");
  if (!outcome.draft.has_value()) {
    return;
  }
  expect(outcome.draft->payloadJson.find("sentinel-api-key") ==
             std::string::npos,
         "payload contains no API key sentinel");
  expect(outcome.draft->payloadJson.find("Authorization") == std::string::npos,
         "payload contains no authorization header");
  expect(outcome.draft->payloadJson.size() <=
             ir::tachi::kMaximumPayloadBytes,
         "payload remains within provider cap");
}

} // namespace

int main() {
  testBuildsOneScoreBatchManual();
  testMapsPlaytypesAndHashFallback();
  testMapsEveryLamp();
  testClampsGauge();
  testRejectsMalformedSubmission();
  testPayloadNeverContainsCredentialMaterial();
  if (failures != 0) {
    std::cerr << failures << " Tachi batch manual test(s) failed\n";
    return 1;
  }
  std::cout << "Tachi batch manual tests passed\n";
  return 0;
}
