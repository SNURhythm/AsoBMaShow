#include "ir/IrUploadCandidates.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::string_view kProvider = "tachi";
constexpr std::string_view kOrigin = "https://boku.tachi.ac";

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

std::string attemptId(int suffix) {
  char value[37]{};
  std::snprintf(value, sizeof(value), "123e4567-e89b-42d3-a456-426614174%03d",
                suffix);
  return value;
}

result_persistence::ModernChartResult modernResult(int suffix) {
  result_persistence::ModernChartResult result;
  result.resultId = suffix;
  result.attemptId = attemptId(suffix);
  result.score.chartPath = "library/chart.bms";
  result.score.chartMd5 = repeated('b', 32);
  result.score.chartSha256 = repeated('a', 64);
  result.score.chartTitle = "Stored title";
  result.score.chartArtist = "Stored artist";
  result.score.longNoteMode = 1;
  result.score.score = 900;
  result.score.maxScore = 1'000;
  result.score.maxCombo = 450;
  result.score.comboBreak = 2;
  result.score.pGreat = 400;
  result.score.great = 100;
  result.score.finalGauge = 82.5F;
  result.score.clearType = kClearTypeHardClearRank;
  bms_parser::ChartMeta meta;
  meta.KeyMode = 7;
  meta.MD5 = result.score.chartMd5;
  meta.SHA256 = result.score.chartSha256;
  meta.Rank = 2;
  meta.TotalNotes = result.score.maxScore / 2;
  meta.HasTotal = true;
  meta.Total = 200.5;
  const auto judge =
      gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2, meta.Rank);
  result.score.provenance = makeScoreProvenance({
      .chartMeta = meta,
      .longNoteMode = result.score.longNoteMode,
      .judgeRankSource = JudgeRankSource::Chart,
      .sourceJudgeRank = meta.Rank,
      .effectiveJudgeContexts = judge.contexts,
      .totalNotes = meta.TotalNotes,
      .authoredGaugeTotal = meta.Total,
      .effectiveGaugeTotal =
          resolveEffectiveGaugeTotal(GameplayRuleset::LR2, meta),
      .candidateSelection = gameplay::CandidateSelectionMode::LR2,
      .gaugeType = GaugeType::Hard,
      .inputDevices = {InputDeviceCategory::Keyboard},
      .ruleset = RulesetDescriptor::For(GameplayRuleset::LR2),
  });
  result.keyMode = 7;
  result.adoptedGaugeType = GaugeType::Hard;
  result.adoptedGaugeHistory = {20.0F, 82.5F};
  result.playedAtUnixMillis = 1'700'000'000'000LL + suffix;
  result.resultFingerprint =
      result_persistence::modernResultFingerprint(result);
  std::string diagnostic;
  assert(result_persistence::validateModernChartResult(result, diagnostic));
  return result;
}

ir::IrSubmissionSnapshot snapshotFor(
    const result_persistence::ModernChartResult &result) {
  std::string diagnostic;
  const auto snapshot = ir::captureIrSubmissionSnapshot(result, diagnostic);
  assert(snapshot.has_value());
  return *snapshot;
}

ir::IrUploadCandidateSource source(int suffix) {
  auto result = modernResult(suffix);
  return {.modernChartResultId = result.resultId,
          .result = result,
          .snapshot = snapshotFor(result)};
}

void refreshSnapshot(ir::IrUploadCandidateSource &value) {
  value.result.resultFingerprint =
      result_persistence::modernResultFingerprint(value.result);
  value.snapshot = snapshotFor(value.result);
}

ir::IrOutboxEntry outboxFor(const ir::IrUploadCandidateSource &source,
                            ir::IrOutboxState state) {
  ir::IrOutboxEntry entry{
      .id = 1'000 + source.modernChartResultId,
      .providerId = std::string(kProvider),
      .attemptId = source.result.attemptId,
      .chartMd5 = source.result.score.chartMd5,
      .chartSha256 = source.result.score.chartSha256,
      .payloadJson = "{}",
      .rulesetProof = {.rulesetId = "test-rules",
                       .rulesetRevision = 1,
                       .validationFingerprint = repeated('d', 64)},
      .state = state,
      .localResultReady = true,
      .lastErrorMessage = state == ir::IrOutboxState::FailedPermanent
                              ? std::string("invalid\x01 payload")
                              : std::string{},
      .createdAtUnixMillis = source.result.playedAtUnixMillis,
      .updatedAtUnixMillis = source.result.playedAtUnixMillis,
  };
  if (state == ir::IrOutboxState::Succeeded) {
    entry.completedAtUnixMillis = source.result.playedAtUnixMillis;
  }
  std::string diagnostic;
  assert(ir::validateIrOutboxEntry(entry, diagnostic));
  return entry;
}

ir::IrSubmissionReceipt receiptFor(
    const ir::IrUploadCandidateSource &source) {
  ir::IrSubmissionReceipt receipt{
      .id = 2'000 + source.modernChartResultId,
      .providerId = std::string(kProvider),
      .serverOrigin = std::string(kOrigin),
      .replayId = 0,
      .modernChartResultId = source.modernChartResultId,
      .attemptId = source.result.attemptId,
      .chartMd5 = source.result.score.chartMd5,
      .chartSha256 = source.result.score.chartSha256,
      .remoteUserId = 42,
      .remoteChartId = "remote-chart",
      .remoteScoreId = "remote-score",
      .confirmedAtUnixMillis = source.result.playedAtUnixMillis,
  };
  std::string diagnostic;
  assert(ir::validateIrSubmissionReceipt(receipt, diagnostic));
  return receipt;
}

struct CountingCandidate {
  std::string id;
  std::size_t *reads = nullptr;

  [[nodiscard]] std::string_view attemptId() const noexcept {
    ++*reads;
    return id;
  }
};

void testSelectionIndexesCanonicalAttemptIdsOnce() {
  constexpr std::size_t candidateCount =
      ir::kMaximumIrUploadCandidateRows;
  std::size_t candidateReads = 0;
  std::vector<CountingCandidate> candidates;
  candidates.reserve(candidateCount);
  std::unordered_set<std::string> selected;
  selected.reserve(candidateCount);

  for (std::size_t index = 0; index < candidateCount; ++index) {
    const std::string id = "attempt-" + std::to_string(index);
    candidates.push_back({.id = id, .reads = &candidateReads});
    selected.insert(index % 2 == 0 ? id : "stale-" + std::to_string(index));
  }

  ir::detail::intersectIrUploadSelectionIndexed(selected, candidates);

  expect(candidateReads == candidateCount,
         "selection indexes each candidate attempt ID exactly once");
  expect(selected.size() == candidateCount / 2,
         "large selection retains only published attempt IDs");
}

void testProjectsOnlySnapshotBackedModernAttempts() {
  auto eligible = source(11);
  auto failed = source(12);
  failed.outbox = outboxFor(failed, ir::IrOutboxState::FailedPermanent);
  auto queued = source(13);
  queued.outbox = outboxFor(queued, ir::IrOutboxState::Pending);
  auto uploaded = source(14);
  uploaded.receipt = receiptFor(uploaded);
  auto mismatched = source(15);
  mismatched.snapshot = source(16).snapshot;
  auto missingOwner = source(17);
  missingOwner.modernChartResultId = 0;

  const std::vector sources{eligible, failed, queued, uploaded, mismatched,
                            missingOwner};
  const auto records = ir::projectIrUploadRecords(sources, kProvider, kOrigin);
  const auto projected =
      ir::projectIrUploadCandidates(sources, kProvider, kOrigin);

  expect(records.records.size() == 4 && records.omittedRows == 2,
         "record projection retains every valid durable IR state");
  expect(records.records[0].resolvedState() == ir::IrRecordState::Eligible &&
             records.records[1].resolvedState() == ir::IrRecordState::Failed &&
             records.records[2].resolvedState() == ir::IrRecordState::Queued &&
             records.records[3].resolvedState() == ir::IrRecordState::Uploaded,
         "record and upload pages share one durable state authority");
  expect(records.records[2].resolvedState(
             ir::IrRecordActivity::Submitting) ==
             ir::IrRecordState::Uploading &&
             records.records[3].resolvedState(
                 ir::IrRecordActivity::Polling) ==
                 ir::IrRecordState::Uploaded,
         "live activity overlays durable state without overriding receipts");
  expect(projected.candidates.size() == 2,
         "only eligible and failed modern snapshots remain selectable");
  expect(projected.candidates[0].attemptId() == eligible.result.attemptId &&
             projected.candidates[1].attemptId() == failed.result.attemptId,
         "projection preserves stable modern attempt identity");
  expect(projected.candidates[0].modernChartResultId ==
                 eligible.modernChartResultId &&
             projected.candidates[0].result == eligible.result &&
             projected.candidates[0].snapshot == eligible.snapshot,
         "candidate display and submission facts come from durable modern rows");
  expect(projected.candidates[1].state == ir::IrRecordState::Failed &&
             projected.candidates[1].failureReason == "invalid  payload",
         "failed outbox work remains retryable with a sanitized reason");
  expect(projected.omittedRows == 2 && !projected.diagnostic.empty(),
         "mismatched snapshot and owner rows fail closed without hiding state");

  std::unordered_set<std::string> selected{
      eligible.result.attemptId, queued.result.attemptId, "stale-attempt"};
  ir::intersectIrUploadSelection(selected, projected.candidates);
  expect(selected ==
             std::unordered_set<std::string>{eligible.result.attemptId},
         "refresh retains only published modern attempt IDs");
}

void testStoredSnapshotSubmissionNeedsNoReplayFileOrChartHydration() {
  const auto projected = ir::projectIrUploadCandidates(
      std::vector{source(21)}, kProvider, kOrigin);
  expect(projected.candidates.size() == 1,
         "snapshot-backed result is selectable without a replay reference");

  std::string diagnostic;
  const auto submission =
      ir::submissionForIrUploadCandidate(projected.candidates.front(),
                                         diagnostic);
  expect(submission.has_value() &&
             *submission == projected.candidates.front().snapshot.submission &&
             submission->attemptId == projected.candidates.front().attemptId(),
         "manual preparation returns the already validated stored submission");

  auto tampered = projected.candidates.front();
  tampered.result.score.score += 1;
  expect(!ir::submissionForIrUploadCandidate(tampered, diagnostic) &&
             !diagnostic.empty(),
         "result/snapshot disagreement fails at the shared preparation boundary");
}

void testProviderEligibilityHidesModifiedModernResults() {
  auto modified = source(31);
  modified.result.score.provenance.eligibility = ScoreEligibility::Modified;
  refreshSnapshot(modified);

  const auto records = ir::projectIrUploadRecords(
      std::vector{modified}, kProvider, kOrigin);
  const auto candidates = ir::projectIrUploadCandidates(
      std::vector{modified}, kProvider, kOrigin);

  expect(records.records.size() == 1 && !records.records.front().eligible &&
             records.records.front().resolvedState() ==
                 ir::IrRecordState::Hidden,
         "provider-ineligible modern snapshots publish no Records upload "
         "action");
  expect(candidates.candidates.empty(),
         "provider-ineligible modern snapshots are not manual upload "
         "candidates");
}

void testProviderNeutralEligibilitySupportsOtherDrivers() {
  auto verified = source(32);
  auto modified = source(33);
  modified.result.score.provenance.eligibility = ScoreEligibility::Modified;
  refreshSnapshot(modified);

  constexpr std::string_view provider = "fake";
  constexpr std::string_view origin = "https://fake.example.test";
  const auto records = ir::projectIrUploadRecords(
      std::vector{verified, modified}, provider, origin);
  const auto candidates = ir::projectIrUploadCandidates(
      std::vector{verified, modified}, provider, origin);

  expect(records.records.size() == 2 && records.records[0].eligible &&
             !records.records[1].eligible,
         "provider-neutral verified provenance remains available to modular "
         "drivers without admitting modified results");
  expect(candidates.candidates.size() == 1 &&
             candidates.candidates.front().attemptId() ==
                 verified.result.attemptId,
         "other drivers receive only provider-neutral eligible snapshots");
}

} // namespace

int main() {
  testSelectionIndexesCanonicalAttemptIdsOnce();
  testProjectsOnlySnapshotBackedModernAttempts();
  testStoredSnapshotSubmissionNeedsNoReplayFileOrChartHydration();
  testProviderEligibilityHidesModifiedModernResults();
  testProviderNeutralEligibilitySupportsOtherDrivers();
  return failures == 0 ? 0 : 1;
}
