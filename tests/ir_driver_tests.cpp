#include "ir/IrDriver.h"
#include "ir/IrHttpClient.h"
#include "ir/IrRankingModels.h"
#include "ir/IrSubmission.h"
#include "ModernResult.h"

#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
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

std::string repeated(char value, std::size_t count) {
  return std::string(count, value);
}

result_persistence::ModernChartResult validResult() {
  result_persistence::ModernChartResult result;
  result.attemptId = "123e4567-e89b-42d3-a456-426614174000";
  result.score.chartPath = "sample/song.bms";
  result.score.chartMd5 = repeated('b', 32);
  result.score.chartSha256 = repeated('a', 64);
  result.score.score = 7;
  result.score.maxScore = 10;
  result.score.maxCombo = 4;
  result.score.comboBreak = 1;
  result.score.pGreat = 3;
  result.score.great = 1;
  result.score.good = 1;
  result.score.bad = 0;
  result.score.poor = 0;
  result.score.kPoor = 0;
  result.score.fast = 2;
  result.score.slow = 1;
  result.score.finalGauge = 82.5F;
  result.score.clearType = kClearTypeNormalClearRank;
  result.score.provenance = ScoreProvenance::Legacy();
  result.keyMode = 7;
  result.adoptedGaugeType = GaugeType::Normal;
  result.playedAtUnixMillis = 1'700'000'000'123LL;
  result.resultFingerprint =
      result_persistence::modernResultFingerprint(result);
  return result;
}

ir::IrSubmissionBuildOutcome buildSubmission(
    result_persistence::ModernChartResult result,
    std::int64_t playedAtUnixMillis = 1'700'000'000'123LL) {
  result.playedAtUnixMillis = playedAtUnixMillis;
  result.resultFingerprint =
      result_persistence::modernResultFingerprint(result);
  return ir::makeIrSubmission(result);
}

class FakeDriver : public ir::IrDriver {
public:
  FakeDriver(std::string id, ir::IrDriverCapabilities capabilities)
      : id_(std::move(id)), capabilities_(capabilities) {}

  std::string_view providerId() const noexcept override { return id_; }
  ir::IrDriverCapabilities capabilities() const noexcept override {
    return capabilities_;
  }

  ir::BuildDraftOutcome buildDraft(const ir::IrSubmission &) const override {
    ++buildCalls;
    return {.status = ir::BuildDraftStatus::Built,
            .draft = ir::IrOutboxDraft{.providerId = id_}};
  }

  ir::DeliveryOutcome submit(const ir::IrOutboxEntry &entry,
                             const ir::IrProviderRuntimeConfig &,
                             ir::IrHttpClient &,
                             std::stop_token) const override {
    submittedEntries.push_back(entry);
    return {.status = ir::DeliveryStatus::Succeeded};
  }

  ir::DeliveryOutcome poll(const ir::IrOutboxEntry &entry,
                           const ir::IrProviderRuntimeConfig &,
                           ir::IrHttpClient &, std::stop_token) const override {
    polledEntries.push_back(entry);
    return {.status = ir::DeliveryStatus::Ongoing};
  }

  ir::IrOutboxBatchPlan
  planBatch(std::span<const ir::IrOutboxEntry> due) const override {
    if (throwDuringPlan) {
      throw std::runtime_error("injected plan failure");
    }
    if (plannedOutcome) {
      return *plannedOutcome;
    }
    if (plannedRowIds) {
      return {.status = ir::IrOutboxBatchPlanStatus::Planned,
              .rowIds = *plannedRowIds};
    }
    return IrDriver::planBatch(due);
  }

  ir::IrUserScoreSnapshotOutcome
  fetchUserScoreSnapshot(const ir::IrProviderRuntimeConfig &,
                         ir::IrHttpClient &, std::stop_token,
                         ir::IrUserScoreProgress progress) const override {
    ++reconciliationCalls;
    if (progress) {
      progress("fake-game", 1, 1);
    }
    return {.status = ir::IrUserScoreSnapshotStatus::Succeeded,
            .snapshot = ir::IrUserScoreSnapshot{}};
  }

  mutable int buildCalls = 0;
  mutable int reconciliationCalls = 0;
  mutable std::vector<ir::IrOutboxEntry> submittedEntries;
  mutable std::vector<ir::IrOutboxEntry> polledEntries;
  std::optional<std::vector<std::int64_t>> plannedRowIds;
  std::optional<ir::IrOutboxBatchPlan> plannedOutcome;
  bool throwDuringPlan = false;

private:
  std::string id_;
  ir::IrDriverCapabilities capabilities_;
};

void testCanonicalSubmissionBuildsFromModernResult() {
  const auto outcome = buildSubmission(validResult());
  expect(outcome.value.has_value(), "valid result builds canonical submission");
  if (!outcome.value.has_value()) {
    return;
  }
  const auto &submission = *outcome.value;
  expect(submission.attemptId == "123e4567-e89b-42d3-a456-426614174000",
         "submission retains attempt identity");
  expect(submission.keyMode == 7, "submission retains key mode");
  expect(submission.chartMd5 == repeated('b', 32), "submission normalizes md5");
  expect(submission.chartSha256 == repeated('a', 64),
         "submission normalizes sha256");
  expect(submission.score == 7 && submission.maxScore == 10,
         "submission retains EX score values");
  expect(submission.gaugeHistory.empty() && submission.pGreatFast == 0 &&
             submission.pGreatSlow == 0 &&
             !submission.judgementTimingBreakdownAvailable,
         "empty replay keeps detailed evidence optional");
  expect(submission.playedAtUnixMillis == 1700000000123LL,
         "submission retains capture time");
}

void testCanonicalSubmissionRejectsInvalidInput() {
  auto attempt = validResult();
  attempt.attemptId = "123E4567-E89B-42D3-A456-426614174000";
  expect(!buildSubmission(attempt).value.has_value(),
         "submission rejects non-canonical attempt id");

  attempt = validResult();
  attempt.score.score = 8;
  expect(!buildSubmission(attempt).value.has_value(),
         "submission rejects inconsistent EX score");

  attempt = validResult();
  attempt.score.chartSha256 = "not-a-hash";
  expect(!buildSubmission(attempt).value.has_value(),
         "submission rejects malformed present hash");

  expect(!buildSubmission(validResult(), 0).value.has_value(),
         "submission rejects nonpositive capture time");
}

void testCanonicalSubmissionUsesCapturedResultMetrics() {
  auto attempt = validResult();
  attempt.score.maxScore = 14;
  attempt.score.bad = 1;
  attempt.score.poor = 1;
  attempt.score.fast = 3;
  attempt.score.slow = 2;
  attempt.adoptedGaugeHistory = {24.0F, 25.5F, 27.0F, 28.0F,
                                 26.0F, 22.0F, 18.0F, 11.0F};
  result_persistence::ChartJudgementTiming timing;
  timing.byJudgement[PGreat] = {.fast = 1, .slow = 1};
  timing.byJudgement[Great] = {.fast = 1, .slow = 0};
  timing.byJudgement[Good] = {.fast = 1, .slow = 0};
  timing.byJudgement[Bad] = {.fast = 0, .slow = 1};
  attempt.judgementTiming = timing;

  const auto outcome = buildSubmission(attempt);
  expect(outcome.value.has_value(), "captured result metrics build");
  if (outcome.value) {
    expect(outcome.value->gaugeHistory == attempt.adoptedGaugeHistory,
           "captured adopted gauge history becomes submission history");
    expect(outcome.value->pGreatFast == 1 && outcome.value->pGreatSlow == 1,
           "PGREAT early and late evidence is separated");
    expect(
        outcome.value->judgementTimingBreakdownAvailable &&
            outcome.value->earlyPGreat == 2 && outcome.value->latePGreat == 1 &&
            outcome.value->earlyGreat == 1 && outcome.value->lateGreat == 0 &&
            outcome.value->earlyGood == 1 && outcome.value->lateGood == 0 &&
            outcome.value->earlyBad == 0 && outcome.value->lateBad == 1 &&
            outcome.value->earlyPoor == 1 && outcome.value->latePoor == 0,
        "captured state exposes every LR2 judgement timing");
  }
}

void testCanonicalSubmissionUsesCapturedTimingForClassicLongNotes() {
  auto attempt = validResult();
  attempt.score.maxScore = 12;
  attempt.score.bad = 1;
  attempt.score.slow = 3;
  attempt.adoptedGaugeHistory = {41.0F, 57.5F, 82.5F};
  result_persistence::ChartJudgementTiming timing;
  timing.byJudgement[PGreat] = {.fast = 1, .slow = 1};
  timing.byJudgement[Great] = {.fast = 0, .slow = 1};
  timing.byJudgement[Good] = {.fast = 1, .slow = 0};
  timing.byJudgement[Bad] = {.fast = 0, .slow = 1};
  attempt.judgementTiming = timing;

  const auto outcome = buildSubmission(attempt);
  expect(outcome.value.has_value(), "classic long-note result builds");
  if (outcome.value) {
    expect(outcome.value->gaugeHistory == attempt.adoptedGaugeHistory,
           "captured adopted gauge history is authoritative");
    expect(outcome.value->pGreatFast == 1 && outcome.value->pGreatSlow == 1,
           "captured PGREAT timing drives fast slow exclusion");
    expect(
        outcome.value->judgementTimingBreakdownAvailable &&
            outcome.value->earlyPGreat == 2 && outcome.value->latePGreat == 1 &&
            outcome.value->earlyGreat == 0 && outcome.value->lateGreat == 1 &&
            outcome.value->earlyGood == 1 && outcome.value->lateGood == 0 &&
            outcome.value->earlyBad == 0 && outcome.value->lateBad == 1,
        "captured result timing ignores informational LN head events");
  }
}

void testCanonicalSubmissionUsesOnlyAdoptedGaugeHistory() {
  auto attempt = validResult();
  auto outcome = buildSubmission(attempt);
  expect(outcome.value && outcome.value->gaugeHistory.empty(),
         "missing adopted history remains absent");
  expect(outcome.value && outcome.value->pGreatFast == 0 &&
             outcome.value->pGreatSlow == 0,
         "missing captured timing remains absent");

  attempt.adoptedGaugeHistory = {41.0F, 57.5F, 82.5F};
  outcome = buildSubmission(attempt);
  expect(outcome.value &&
             outcome.value->gaugeHistory == attempt.adoptedGaugeHistory,
         "durable adopted history becomes the submission history");
}

void testCanonicalSubmissionRejectsInvalidDetailedEvidence() {
  auto attempt = validResult();
  attempt.adoptedGaugeHistory = {std::numeric_limits<float>::quiet_NaN()};
  expect(!buildSubmission(attempt).value,
         "non-finite captured gauge history is rejected");

  attempt = validResult();
  attempt.score.fast = 0;
  result_persistence::ChartJudgementTiming timing;
  timing.byJudgement[PGreat] = {.fast = 1, .slow = 1};
  attempt.judgementTiming = timing;
  expect(!buildSubmission(attempt).value,
         "captured timing must match aggregate timing");

  attempt = validResult();
  attempt.score.fast = 2;
  attempt.score.slow = 0;
  timing = {};
  timing.byJudgement[Good] = {.fast = 2, .slow = 0};
  attempt.judgementTiming = timing;
  expect(!buildSubmission(attempt).value,
         "captured timing cannot exceed its judgement total");
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
    expect(outcome.value->totalNotes == 1234, "query retains total notes");
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

void testIrBadPointsIncludeKpoor() {
  expect(ir::calculateIrBadPoints(14, 8, 40) == 62,
         "IR BP includes BAD, POOR, and KPOOR");
  expect(!ir::calculateIrBadPoints(-1, 8, 40).has_value(),
         "IR BP rejects negative counters");
  expect(!ir::calculateIrBadPoints(std::numeric_limits<int>::max(), 1, 0)
              .has_value(),
         "IR BP rejects integer overflow");
}

void testCapabilityValidation() {
  expect(ir::validateCapabilities({.readOnly = true, .chartRankings = true}),
         "read-only ranking driver is valid");
  expect(ir::validateCapabilities({.chartRankings = true,
                                   .scoreSubmission = true,
                                   .deferredSubmission = true,
                                   .scoreReconciliation = true}),
         "read-write deferred driver is valid");
  expect(
      ir::validateCapabilities({.readOnly = true, .scoreReconciliation = true}),
      "read-only reconciliation driver is valid");
  expect(!ir::validateCapabilities({}), "driver must expose an operation");
  expect(
      !ir::validateCapabilities(
          {.readOnly = true, .chartRankings = true, .scoreSubmission = true}),
      "read-only driver cannot submit");
  expect(!ir::validateCapabilities(
             {.chartRankings = true, .deferredSubmission = true}),
         "deferred capability requires submission");
}

class NoopHttpClient final : public ir::IrHttpClient {
public:
  ir::IrHttpResponse perform(const ir::IrHttpRequest &,
                             std::stop_token) noexcept override {
    return {};
  }
};

void testGenericBatchFallbacksPreserveSingularCompatibility() {
  FakeDriver driver("submitter",
                    {.scoreSubmission = true, .deferredSubmission = true});
  std::vector<ir::IrOutboxEntry> due(2);
  due[0].id = 11;
  due[1].id = 12;

  const auto plan = driver.ir::IrDriver::planBatch(due);
  expect(plan.status == ir::IrOutboxBatchPlanStatus::Planned &&
             plan.rowIds == std::vector<std::int64_t>{11},
         "base batch plan selects the first due row only");

  auto withInvalidFirst = due;
  withInvalidFirst.front().id = 0;
  const auto firstValid =
      driver.ir::IrDriver::planBatch(withInvalidFirst);
  expect(firstValid.status == ir::IrOutboxBatchPlanStatus::Planned &&
             firstValid.rowIds == std::vector<std::int64_t>{12},
         "base batch plan skips invalid rows before its singular fallback");

  NoopHttpClient http;
  const auto submitted = driver.ir::IrDriver::submitBatch(
      std::span<const ir::IrOutboxEntry>(due).first(1), true, {}, http, {});
  expect(submitted.status == ir::DeliveryStatus::Succeeded &&
             driver.submittedEntries.size() == 1 &&
             driver.submittedEntries.front().nextRequestUserIntent,
         "base batch submission copies one row and applies user intent");

  const auto polled = driver.ir::IrDriver::pollBatch(
      std::span<const ir::IrOutboxEntry>(due).first(1), {}, http, {});
  expect(polled.status == ir::DeliveryStatus::Ongoing &&
             driver.polledEntries.size() == 1,
         "base batch polling delegates exactly one row");

  const auto rejected =
      driver.ir::IrDriver::submitBatch(due, false, {}, http, {});
  expect(rejected.status == ir::DeliveryStatus::PermanentFailure &&
             driver.submittedEntries.size() == 1,
         "base batch submission rejects multiple rows without delivery");
}

void testRegistryRejectsInvalidDriverBatchPlans() {
  ir::IrDriverRegistry registry;
  auto driver = std::make_shared<FakeDriver>(
      "submitter", ir::IrDriverCapabilities{.scoreSubmission = true});
  std::string diagnostic;
  expect(registry.registerDriver(driver, diagnostic),
         "batch plan test driver registers");
  std::vector<ir::IrOutboxEntry> due(2);
  due[0].id = 21;
  due[1].id = 22;

  driver->plannedRowIds = std::vector<std::int64_t>{21, 21};
  expect(registry.planBatch("submitter", due).status ==
             ir::IrOutboxBatchPlanStatus::Invalid,
         "registry rejects duplicate planned row IDs");

  driver->plannedRowIds = std::vector<std::int64_t>{0};
  expect(registry.planBatch("submitter", due).status ==
             ir::IrOutboxBatchPlanStatus::Invalid,
         "registry rejects unknown planned row IDs");

  driver->plannedRowIds = std::vector<std::int64_t>{99};
  expect(registry.planBatch("submitter", due).status ==
             ir::IrOutboxBatchPlanStatus::Invalid,
         "registry rejects row IDs outside the due input");

  driver->plannedRowIds = std::vector<std::int64_t>{21, 22};
  const auto valid = registry.planBatch("submitter", due);
  expect(valid.status == ir::IrOutboxBatchPlanStatus::Planned &&
             valid.rowIds == std::vector<std::int64_t>({21, 22}),
         "registry preserves a valid driver batch plan");

  driver->plannedRowIds.reset();
  driver->plannedOutcome = ir::IrOutboxBatchPlan{
      .status = ir::IrOutboxBatchPlanStatus::Invalid,
      .rejectedRowId = 22,
      .diagnostic = "row 22 is malformed",
  };
  const auto identified = registry.planBatch("submitter", due);
  expect(identified.status == ir::IrOutboxBatchPlanStatus::Invalid &&
             identified.rejectedRowId == 22,
         "registry preserves an explicitly identified due-row rejection");

  driver->plannedOutcome->rejectedRowId = 99;
  const auto unknownOffender = registry.planBatch("submitter", due);
  expect(unknownOffender.status == ir::IrOutboxBatchPlanStatus::Invalid &&
             !unknownOffender.rejectedRowId,
         "registry strips an out-of-input rejected row identity");

  driver->plannedOutcome.reset();
  driver->throwDuringPlan = true;
  const auto exception = registry.planBatch("submitter", due);
  expect(exception.status == ir::IrOutboxBatchPlanStatus::Invalid &&
             !exception.rejectedRowId,
         "registry exceptions never accuse an arbitrary due row");
}

void testRegistryForwardsScoreReconciliation() {
  ir::IrDriverRegistry registry;
  auto driver = std::make_shared<FakeDriver>(
      "archive",
      ir::IrDriverCapabilities{.readOnly = true, .scoreReconciliation = true});
  std::string diagnostic;
  expect(registry.registerDriver(driver, diagnostic),
         "reconciliation-only driver registers");
  NoopHttpClient http;
  int progressCalls = 0;
  const auto result = registry.fetchUserScoreSnapshot(
      "archive", {}, http, {},
      [&](std::string_view, int, int) { ++progressCalls; });
  expect(result.status == ir::IrUserScoreSnapshotStatus::Succeeded &&
             result.snapshot && driver->reconciliationCalls == 1 &&
             progressCalls == 1,
         "registry forwards reconciliation and progress to the driver");

  const auto unsupported =
      registry.fetchUserScoreSnapshot("missing", {}, http, {}, {});
  expect(unsupported.status == ir::IrUserScoreSnapshotStatus::Unsupported &&
             !unsupported.snapshot,
         "registry rejects reconciliation for an unregistered provider");
}

void testRegistryRejectsInvalidAndDuplicateDrivers() {
  ir::IrDriverRegistry registry;
  std::string diagnostic;
  expect(
      !registry.registerDriver(
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
      "archive",
      ir::IrDriverCapabilities{.readOnly = true, .chartRankings = true});
  std::string diagnostic;
  expect(registry.registerDriver(driver, diagnostic),
         "read-only driver registers");
  const auto submission = buildSubmission(validResult());
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
  const auto submission = buildSubmission(validResult());
  expect(submission.value.has_value(),
         "test submission is valid for base call");
  if (!submission.value.has_value()) {
    return;
  }
  const auto outcome = driver.ir::IrDriver::buildDraft(*submission.value);
  expect(outcome.status == ir::BuildDraftStatus::Unsupported,
         "base build draft returns unsupported");
}

} // namespace

int main() {
  testCanonicalSubmissionBuildsFromModernResult();
  testCanonicalSubmissionRejectsInvalidInput();
  testCanonicalSubmissionUsesCapturedResultMetrics();
  testCanonicalSubmissionUsesCapturedTimingForClassicLongNotes();
  testCanonicalSubmissionUsesOnlyAdoptedGaugeHistory();
  testCanonicalSubmissionRejectsInvalidDetailedEvidence();
  testChartQueryNormalizesIdentity();
  testIrBadPointsIncludeKpoor();
  testCapabilityValidation();
  testGenericBatchFallbacksPreserveSingularCompatibility();
  testRegistryRejectsInvalidDriverBatchPlans();
  testRegistryForwardsScoreReconciliation();
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
