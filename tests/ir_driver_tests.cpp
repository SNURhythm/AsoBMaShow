#include "ir/IrDriver.h"
#include "ir/IrRankingModels.h"
#include "ir/IrSubmission.h"

#include <iostream>
#include <memory>
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

result_persistence::ChartResultAttempt validAttempt() {
  result_persistence::ChartResultAttempt attempt;
  attempt.attemptId = "123e4567-e89b-42d3-a456-426614174000";
  attempt.replay.chartMeta.KeyMode = 7;
  attempt.replay.chartMeta.MD5 = repeated('B', 32);
  attempt.replay.chartMeta.SHA256 = repeated('A', 64);
  attempt.replay.chartMeta.TotalNotes = 5;
  attempt.score.chartMd5 = repeated('b', 32);
  attempt.score.chartSha256 = repeated('a', 64);
  attempt.score.score = 7;
  attempt.score.maxScore = 10;
  attempt.score.maxCombo = 4;
  attempt.score.comboBreak = 1;
  attempt.score.pGreat = 3;
  attempt.score.great = 1;
  attempt.score.good = 1;
  attempt.score.bad = 0;
  attempt.score.poor = 0;
  attempt.score.kPoor = 0;
  attempt.score.fast = 2;
  attempt.score.slow = 1;
  attempt.score.finalGauge = 82.5F;
  attempt.score.clearType = kClearTypeNormalClearRank;
  attempt.score.provenance = ScoreProvenance::Legacy();
  return attempt;
}

class FakeDriver final : public ir::IrDriver {
public:
  FakeDriver(std::string id, ir::IrDriverCapabilities capabilities)
      : id_(std::move(id)), capabilities_(capabilities) {}

  std::string_view providerId() const noexcept override { return id_; }
  ir::IrDriverCapabilities capabilities() const noexcept override {
    return capabilities_;
  }

  ir::BuildDraftOutcome
  buildDraft(const ir::IrSubmission &) const override {
    ++buildCalls;
    return {.status = ir::BuildDraftStatus::Built,
            .draft = ir::IrOutboxDraft{.providerId = id_}};
  }

  mutable int buildCalls = 0;

private:
  std::string id_;
  ir::IrDriverCapabilities capabilities_;
};

void testCanonicalSubmissionBuildsFromAttempt() {
  const auto outcome = ir::makeIrSubmission(validAttempt(), 1700000000123LL);
  expect(outcome.value.has_value(), "valid result builds canonical submission");
  if (!outcome.value.has_value()) {
    return;
  }
  const auto &submission = *outcome.value;
  expect(submission.attemptId ==
             "123e4567-e89b-42d3-a456-426614174000",
         "submission retains attempt identity");
  expect(submission.keyMode == 7, "submission retains key mode");
  expect(submission.chartMd5 == repeated('b', 32),
         "submission normalizes md5");
  expect(submission.chartSha256 == repeated('a', 64),
         "submission normalizes sha256");
  expect(submission.score == 7 && submission.maxScore == 10,
         "submission retains EX score values");
  expect(submission.playedAtUnixMillis == 1700000000123LL,
         "submission retains capture time");
}

void testCanonicalSubmissionRejectsInvalidInput() {
  auto attempt = validAttempt();
  attempt.attemptId = "123E4567-E89B-42D3-A456-426614174000";
  expect(!ir::makeIrSubmission(attempt, 1700000000123LL).value.has_value(),
         "submission rejects non-canonical attempt id");

  attempt = validAttempt();
  attempt.score.score = 8;
  expect(!ir::makeIrSubmission(attempt, 1700000000123LL).value.has_value(),
         "submission rejects inconsistent EX score");

  attempt = validAttempt();
  attempt.score.chartSha256 = "not-a-hash";
  expect(!ir::makeIrSubmission(attempt, 1700000000123LL).value.has_value(),
         "submission rejects malformed present hash");

  expect(!ir::makeIrSubmission(validAttempt(), 0).value.has_value(),
         "submission rejects nonpositive capture time");
}

void testChartQueryNormalizesIdentity() {
  bms_parser::ChartMeta meta;
  meta.KeyMode = 14;
  meta.MD5 = "  " + repeated('B', 32) + " ";
  meta.SHA256 = " " + repeated('A', 64) + "  ";
  meta.TotalNotes = 1234;

  const auto outcome = ir::makeIrChartQuery(meta);
  expect(outcome.value.has_value(), "valid chart builds ranking query");
  if (outcome.value.has_value()) {
    expect(outcome.value->keyMode == 14, "query retains key mode");
    expect(outcome.value->chartMd5 == repeated('b', 32),
           "query normalizes md5");
    expect(outcome.value->chartSha256 == repeated('a', 64),
           "query normalizes sha256");
    expect(outcome.value->totalNotes == 1234,
           "query retains total notes");
  }

  meta.TotalNotes = 0;
  expect(!ir::makeIrChartQuery(meta).value.has_value(),
         "query rejects nonpositive notes");
  meta.TotalNotes = 1234;
  meta.SHA256.clear();
  meta.MD5.clear();
  expect(!ir::makeIrChartQuery(meta).value.has_value(),
         "query rejects missing identity");
}

void testCapabilityValidation() {
  expect(ir::validateCapabilities({.readOnly = true, .chartRankings = true}),
         "read-only ranking driver is valid");
  expect(ir::validateCapabilities({.chartRankings = true,
                                   .scoreSubmission = true,
                                   .deferredSubmission = true}),
         "read-write deferred driver is valid");
  expect(!ir::validateCapabilities({}), "driver must expose an operation");
  expect(!ir::validateCapabilities({.readOnly = true,
                                    .chartRankings = true,
                                    .scoreSubmission = true}),
         "read-only driver cannot submit");
  expect(!ir::validateCapabilities({.chartRankings = true,
                                    .deferredSubmission = true}),
         "deferred capability requires submission");
}

void testRegistryRejectsInvalidAndDuplicateDrivers() {
  ir::IrDriverRegistry registry;
  std::string diagnostic;
  expect(!registry.registerDriver(
             std::make_shared<FakeDriver>(
                 "Bad Provider", ir::IrDriverCapabilities{.chartRankings = true}),
             diagnostic),
         "registry rejects invalid provider id");
  expect(!diagnostic.empty(), "invalid provider has diagnostic");

  diagnostic.clear();
  expect(registry.registerDriver(
             std::make_shared<FakeDriver>(
                 "tachi", ir::IrDriverCapabilities{.chartRankings = true}),
             diagnostic),
         "registry accepts valid provider");
  expect(!registry.registerDriver(
             std::make_shared<FakeDriver>(
                 "tachi", ir::IrDriverCapabilities{.scoreSubmission = true}),
             diagnostic),
         "registry rejects duplicate provider id");
}

void testReadOnlyDriverCannotBuildSubmissionDraft() {
  ir::IrDriverRegistry registry;
  auto driver = std::make_shared<FakeDriver>(
      "archive", ir::IrDriverCapabilities{.readOnly = true,
                                           .chartRankings = true});
  std::string diagnostic;
  expect(registry.registerDriver(driver, diagnostic),
         "read-only driver registers");
  const auto submission = ir::makeIrSubmission(validAttempt(), 1700000000123LL);
  expect(submission.value.has_value(), "test submission is valid");
  if (!submission.value.has_value()) {
    return;
  }

  const auto outcome = registry.buildDraft("archive", *submission.value);
  expect(outcome.status == ir::BuildDraftStatus::Unsupported,
         "read-only build is unsupported");
  std::map<std::string, ir::IrProviderSettings> settings;
  settings["archive"] = {.enabled = true,
                         .autoSubmit = true,
                         .serverOrigin = "https://archive.example"};
  expect(registry.buildAutomaticDrafts(settings, *submission.value).empty(),
         "automatic capture excludes a malicious read-only driver");
  expect(driver->buildCalls == 0,
         "registry does not invoke read-only build implementation");
}

void testBaseBuildDraftIsUnsupported() {
  FakeDriver driver("ranking", {.chartRankings = true});
  const auto submission = ir::makeIrSubmission(validAttempt(), 1700000000123LL);
  expect(submission.value.has_value(), "test submission is valid for base call");
  if (!submission.value.has_value()) {
    return;
  }
  const auto outcome = driver.ir::IrDriver::buildDraft(*submission.value);
  expect(outcome.status == ir::BuildDraftStatus::Unsupported,
         "base build draft returns unsupported");
}

} // namespace

int main() {
  testCanonicalSubmissionBuildsFromAttempt();
  testCanonicalSubmissionRejectsInvalidInput();
  testChartQueryNormalizesIdentity();
  testCapabilityValidation();
  testRegistryRejectsInvalidAndDuplicateDrivers();
  testReadOnlyDriverCannotBuildSubmissionDraft();
  testBaseBuildDraftIsUnsupported();
  if (failures != 0) {
    std::cerr << failures << " IR driver test(s) failed\n";
    return 1;
  }
  std::cout << "IR driver tests passed\n";
  return 0;
}
