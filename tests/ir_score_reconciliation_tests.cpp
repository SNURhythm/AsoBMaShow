#include "../src/ir/IrScoreReconciliation.h"

#include "../src/scene/play/GameplayGaugeTypes.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr std::string_view kProvider = "tachi";
constexpr std::string_view kOrigin = "https://boku.tachi.ac";
constexpr std::int64_t kConfirmedAt = 5'000;

ir::IrRemoteScore remoteScore(std::string id, char md5Digit = 'b',
                              char shaDigit = 'a', int score = 150,
                              int lampRank = kClearTypeNormalClearRank,
                              std::string game = "bms-7k") {
  return {
      .remoteUserId = 42,
      .game = std::move(game),
      .remoteScoreId = std::move(id),
      .remoteChartId = "remote-chart",
      .chartMd5 = std::string(32, md5Digit),
      .chartSha256 = std::string(64, shaDigit),
      .title = "Remote title",
      .artist = "Remote artist",
      .service = "Bokutachi",
      .noteCount = 100,
      .score = score,
      .lampRank = lampRank,
      .timeAddedUnixMillis = 1'000,
  };
}

ir::IrLocalReceiptCandidate localCandidate(int suffix = 1) {
  assert(suffix >= 0 && suffix < 16);
  std::string attemptId = "00000000-0000-4000-8000-000000000000";
  attemptId.back() = "0123456789abcdef"[suffix];
  return {
      .modernChartResultId = suffix,
      .attemptId = std::move(attemptId),
      .keyMode = 7,
      .chartMd5 = std::string(32, 'b'),
      .chartSha256 = std::string(64, 'a'),
      .score = 150,
      .lampRank = kClearTypeNormalClearRank,
      .eligible = true,
  };
}

ir::IrSubmissionReceipt receiptFor(const ir::IrLocalReceiptCandidate &local,
                                   std::string remoteScoreId,
                                   bool observed = false) {
  return {
      .id = 100 + local.modernChartResultId,
      .providerId = std::string(kProvider),
      .serverOrigin = std::string(kOrigin),
      .replayId = 0,
      .modernChartResultId = local.modernChartResultId,
      .attemptId = local.attemptId,
      .chartMd5 = local.chartMd5,
      .chartSha256 = local.chartSha256,
      .remoteUserId = 42,
      .remoteChartId = "old-chart",
      .remoteScoreId = std::move(remoteScoreId),
      .source = ir::IrReceiptConfirmationSource::Submission,
      .observedInSnapshot = observed,
      .confirmedAtUnixMillis = 2'000,
  };
}

void testExactRemoteIdIsObserved() {
  auto local = localCandidate();
  local.currentReceipt = receiptFor(local, "remote-exact");
  const std::vector remote{
      remoteScore("remote-exact", 'd', 'c', 120, kClearTypeHardClearRank)};

  const auto plan = ir::planScoreReconciliation(
      kProvider, kOrigin, std::span{&local, 1}, remote, kConfirmedAt);

  assert(plan.status == ir::IrScoreReconciliationPlan::Status::Planned);
  assert(plan.upsertedReceipts.size() == 1);
  const auto &upsert = plan.upsertedReceipts.front();
  assert(upsert.id == local.currentReceipt->id);
  assert(upsert.remoteScoreId == "remote-exact");
  assert(upsert.remoteChartId == "remote-chart");
  assert(upsert.remoteUserId == 42);
  assert(upsert.observedInSnapshot);
  assert(upsert.confirmedAtUnixMillis == kConfirmedAt);
  assert(plan.deletedReceiptIds.empty());
}

void testEligibleLocalProofCreatesSnapshotReceipt() {
  auto local = localCandidate(2);
  const std::vector remote{remoteScore("remote-proof")};

  const auto plan = ir::planScoreReconciliation(
      kProvider, kOrigin, std::span{&local, 1}, remote, kConfirmedAt);

  assert(plan.status == ir::IrScoreReconciliationPlan::Status::Planned);
  assert(plan.upsertedReceipts.size() == 1);
  const auto &receipt = plan.upsertedReceipts.front();
  assert(receipt.id == 0);
  assert(receipt.providerId == kProvider);
  assert(receipt.serverOrigin == kOrigin);
  assert(receipt.replayId == 0);
  assert(receipt.modernChartResultId == local.modernChartResultId);
  assert(receipt.attemptId == local.attemptId);
  assert(receipt.chartMd5 == local.chartMd5);
  assert(receipt.chartSha256 == local.chartSha256);
  assert(receipt.remoteUserId == 42);
  assert(receipt.remoteChartId == "remote-chart");
  assert(receipt.remoteScoreId == "remote-proof");
  assert(receipt.source == ir::IrReceiptConfirmationSource::Snapshot);
  assert(receipt.observedInSnapshot);
  assert(receipt.confirmedAtUnixMillis == kConfirmedAt);
}

void testForeignReceiptBoundaryIsNeverMutatedOrReused() {
  auto local = localCandidate(3);
  local.currentReceipt = receiptFor(local, "remote-foreign");
  local.currentReceipt->providerId = "other";
  const std::vector remote{remoteScore("remote-foreign")};

  const auto plan = ir::planScoreReconciliation(
      kProvider, kOrigin, std::span{&local, 1}, remote, kConfirmedAt);

  assert(plan.status == ir::IrScoreReconciliationPlan::Status::Planned);
  assert(plan.upsertedReceipts.empty());
  assert(plan.deletedReceiptIds.empty());
  assert(plan.ambiguousReceiptsPreserved == 0);
}

void testDisappearedSnapshotReceiptIsDeleted() {
  auto local = localCandidate(4);
  local.currentReceipt = receiptFor(local, "remote-disappeared", true);
  local.outboxRowId = 204;
  local.outboxState = ir::IrOutboxState::Succeeded;

  const auto plan = ir::planScoreReconciliation(
      kProvider, kOrigin, std::span{&local, 1}, {}, kConfirmedAt);

  assert(plan.status == ir::IrScoreReconciliationPlan::Status::Planned);
  assert(plan.upsertedReceipts.empty());
  assert(plan.deletedReceiptIds ==
         std::vector<std::int64_t>{local.currentReceipt->id});
  assert(plan.purgedSucceededOutboxRowIds ==
         std::vector<std::int64_t>{204});
  assert(plan.ambiguousReceiptsPreserved == 0);
}

void testUnobservedSubmissionReceiptIsPreservedWhenProofIsAbsent() {
  auto local = localCandidate(5);
  local.currentReceipt = receiptFor(local, "remote-unobserved", false);

  const auto plan = ir::planScoreReconciliation(
      kProvider, kOrigin, std::span{&local, 1}, {}, kConfirmedAt);

  assert(plan.status == ir::IrScoreReconciliationPlan::Status::Planned);
  assert(plan.upsertedReceipts.empty());
  assert(plan.deletedReceiptIds.empty());
  assert(plan.ambiguousReceiptsPreserved == 1);
}

void testMatchedPendingOutboxIsSettled() {
  auto local = localCandidate(6);
  local.outboxRowId = 206;
  local.outboxState = ir::IrOutboxState::Pending;
  const std::vector remote{remoteScore("remote-pending")};

  const auto plan = ir::planScoreReconciliation(
      kProvider, kOrigin, std::span{&local, 1}, remote, kConfirmedAt);

  assert(plan.status == ir::IrScoreReconciliationPlan::Status::Planned);
  assert(plan.upsertedReceipts.size() == 1);
  assert(plan.settledOutboxRowIds == std::vector<std::int64_t>{206});
  assert(plan.purgedSucceededOutboxRowIds.empty());
}

void testUploadingOutboxFailsPlannerValidation() {
  auto local = localCandidate(7);
  local.outboxRowId = 207;
  local.outboxState = ir::IrOutboxState::Uploading;
  const std::vector remote{remoteScore("remote-active")};

  const auto plan = ir::planScoreReconciliation(
      kProvider, kOrigin, std::span{&local, 1}, remote, kConfirmedAt);

  assert(plan.status == ir::IrScoreReconciliationPlan::Status::Invalid);
  assert(!plan.diagnostic.empty());
  assert(plan.upsertedReceipts.empty());
  assert(plan.deletedReceiptIds.empty());
  assert(plan.settledOutboxRowIds.empty());
  assert(plan.purgedSucceededOutboxRowIds.empty());
}

void testAwaitingRemoteResultDoesNotBlockUnrelatedReconciliation() {
  auto deferred = localCandidate(7);
  deferred.outboxRowId = 207;
  deferred.outboxState = ir::IrOutboxState::AwaitingRemoteResult;
  auto unrelated = localCandidate(8);
  unrelated.chartMd5 = std::string(32, 'd');
  unrelated.chartSha256 = std::string(64, 'c');
  const std::vector local{deferred, unrelated};
  const std::vector remote{remoteScore("remote-unrelated", 'd', 'c')};

  const auto plan = ir::planScoreReconciliation(kProvider, kOrigin, local,
                                                remote, kConfirmedAt);

  assert(plan.status == ir::IrScoreReconciliationPlan::Status::Planned);
  assert(plan.upsertedReceipts.size() == 1);
  assert(plan.upsertedReceipts.front().replayId == 0);
  assert(plan.upsertedReceipts.front().modernChartResultId ==
         unrelated.modernChartResultId);
  assert(plan.settledOutboxRowIds.empty());
  assert(plan.purgedSucceededOutboxRowIds.empty());
}

void testPlanMutationsHaveDeterministicIdentityOrder() {
  auto higher = localCandidate(9);
  higher.outboxRowId = 309;
  higher.outboxState = ir::IrOutboxState::Pending;
  auto lower = localCandidate(8);
  lower.currentReceipt = receiptFor(lower, "remote-lower");
  lower.outboxRowId = 308;
  lower.outboxState = ir::IrOutboxState::Succeeded;
  const std::vector local{higher, lower};
  const std::vector remote{remoteScore("remote-higher"),
                           remoteScore("remote-lower", 'd', 'c')};

  const auto plan = ir::planScoreReconciliation(kProvider, kOrigin, local,
                                                remote, kConfirmedAt);

  assert(plan.status == ir::IrScoreReconciliationPlan::Status::Planned);
  assert(plan.upsertedReceipts.size() == 2);
  assert(plan.upsertedReceipts[0].modernChartResultId == 8);
  assert(plan.upsertedReceipts[1].modernChartResultId == 9);
  assert(plan.settledOutboxRowIds == std::vector<std::int64_t>{309});
  assert(plan.purgedSucceededOutboxRowIds == std::vector<std::int64_t>{308});
}

void testPlannerRejectsInvalidIdentityTimeAndSnapshotRows() {
  const auto local = localCandidate(10);
  const std::vector remote{remoteScore("remote-valid")};

  const auto badOrigin =
      ir::planScoreReconciliation(kProvider, "HTTPS://BOKU.TACHI.AC:443/",
                                  std::span{&local, 1}, remote, kConfirmedAt);
  assert(badOrigin.status == ir::IrScoreReconciliationPlan::Status::Invalid);

  const auto badTime = ir::planScoreReconciliation(
      kProvider, kOrigin, std::span{&local, 1}, remote, 0);
  assert(badTime.status == ir::IrScoreReconciliationPlan::Status::Invalid);

  auto malformedRemote = remote.front();
  malformedRemote.game = "bms-9k";
  const auto badRemote =
      ir::planScoreReconciliation(kProvider, kOrigin, std::span{&local, 1},
                                  std::span{&malformedRemote, 1}, kConfirmedAt);
  assert(badRemote.status == ir::IrScoreReconciliationPlan::Status::Invalid);
}

void testProofMatchingEnforcesHashAndGameBoundaries() {
  auto unknownMode = localCandidate(11);
  unknownMode.keyMode = 0;
  auto only14 = remoteScore("remote-14-only", 'b', 'a', 150,
                            kClearTypeNormalClearRank, "bms-14k");
  auto plan = ir::planScoreReconciliation(kProvider, kOrigin,
                                          std::span{&unknownMode, 1},
                                          std::span{&only14, 1}, kConfirmedAt);
  assert(plan.upsertedReceipts.size() == 1 &&
         plan.upsertedReceipts.front().remoteScoreId == "remote-14-only");

  auto same7 = remoteScore("remote-7", 'b', 'a');
  auto same14 = remoteScore("remote-14", 'b', 'a', 150,
                            kClearTypeNormalClearRank, "bms-14k");
  const std::vector bothModes{same14, same7};
  plan = ir::planScoreReconciliation(
      kProvider, kOrigin, std::span{&unknownMode, 1}, bothModes, kConfirmedAt);
  assert(plan.upsertedReceipts.empty());

  unknownMode.keyMode = 7;
  plan = ir::planScoreReconciliation(
      kProvider, kOrigin, std::span{&unknownMode, 1}, bothModes, kConfirmedAt);
  assert(plan.upsertedReceipts.size() == 1 &&
         plan.upsertedReceipts.front().remoteScoreId == "remote-7");

  auto shaDisagrees = same7;
  shaDisagrees.remoteScoreId = "sha-disagrees";
  shaDisagrees.chartSha256 = std::string(64, 'c');
  plan = ir::planScoreReconciliation(kProvider, kOrigin,
                                     std::span{&unknownMode, 1},
                                     std::span{&shaDisagrees, 1}, kConfirmedAt);
  assert(plan.upsertedReceipts.empty());

  auto md5Disagrees = same7;
  md5Disagrees.remoteScoreId = "md5-disagrees";
  md5Disagrees.chartMd5 = std::string(32, 'd');
  plan = ir::planScoreReconciliation(kProvider, kOrigin,
                                     std::span{&unknownMode, 1},
                                     std::span{&md5Disagrees, 1}, kConfirmedAt);
  assert(plan.upsertedReceipts.empty());

  const std::vector poisonedProof{same7, md5Disagrees};
  plan = ir::planScoreReconciliation(kProvider, kOrigin,
                                     std::span{&unknownMode, 1}, poisonedProof,
                                     kConfirmedAt);
  assert(plan.upsertedReceipts.empty());

  unknownMode.chartSha256.clear();
  plan = ir::planScoreReconciliation(kProvider, kOrigin,
                                     std::span{&unknownMode, 1},
                                     std::span{&same7, 1}, kConfirmedAt);
  assert(plan.upsertedReceipts.size() == 1 &&
         plan.upsertedReceipts.front().chartSha256 == same7.chartSha256);
}

void testAmbiguousAndIneligibleProofsNeverCreateReceipts() {
  auto local = localCandidate(12);
  auto first = remoteScore("duplicate-proof-a");
  auto second = remoteScore("duplicate-proof-b");
  const std::vector ambiguous{first, second};
  auto plan = ir::planScoreReconciliation(
      kProvider, kOrigin, std::span{&local, 1}, ambiguous, kConfirmedAt);
  assert(plan.upsertedReceipts.empty());

  local.eligible = false;
  plan = ir::planScoreReconciliation(kProvider, kOrigin, std::span{&local, 1},
                                     std::span{&first, 1}, kConfirmedAt);
  assert(plan.upsertedReceipts.empty());
}

void testUnobservedSubmissionReceiptUsesUniqueProofWithoutChangingSource() {
  auto local = localCandidate(12);
  local.currentReceipt = receiptFor(local, "old-unobserved-id", false);
  auto remote = remoteScore("new-proof-id");
  remote.title = "Different optional display title";
  remote.timeAchievedUnixMillis = 777;
  remote.judgements.pGreat = 1;

  const auto plan =
      ir::planScoreReconciliation(kProvider, kOrigin, std::span{&local, 1},
                                  std::span{&remote, 1}, kConfirmedAt);

  assert(plan.upsertedReceipts.size() == 1);
  assert(plan.upsertedReceipts.front().id == local.currentReceipt->id);
  assert(plan.upsertedReceipts.front().remoteScoreId == "new-proof-id");
  assert(plan.upsertedReceipts.front().source ==
         ir::IrReceiptConfirmationSource::Submission);
  assert(plan.upsertedReceipts.front().observedInSnapshot);
  assert(plan.ambiguousReceiptsPreserved == 0);
}

void testIdenticalLocalProofsShareOneDeterministicRemoteIdentity() {
  auto first = localCandidate(13);
  auto second = localCandidate(14);
  const std::vector local{second, first};
  auto remote = remoteScore("shared-remote");

  const auto plan = ir::planScoreReconciliation(
      kProvider, kOrigin, local, std::span{&remote, 1}, kConfirmedAt);

  assert(plan.upsertedReceipts.size() == 2);
  assert(plan.upsertedReceipts[0].modernChartResultId ==
             first.modernChartResultId &&
         plan.upsertedReceipts[1].modernChartResultId ==
             second.modernChartResultId);
  assert(plan.upsertedReceipts[0].remoteScoreId == "shared-remote" &&
         plan.upsertedReceipts[1].remoteScoreId == "shared-remote");
}

void testOutboxRowsRequireReceiptResolutionBeforeRemoval() {
  auto local = localCandidate(15);
  local.outboxRowId = 415;
  local.outboxState = ir::IrOutboxState::Succeeded;

  auto plan = ir::planScoreReconciliation(
      kProvider, kOrigin, std::span{&local, 1}, {}, kConfirmedAt);
  assert(plan.upsertedReceipts.empty());
  assert(plan.purgedSucceededOutboxRowIds.empty());

  auto remote = remoteScore("remote-succeeded");
  plan = ir::planScoreReconciliation(kProvider, kOrigin, std::span{&local, 1},
                                     std::span{&remote, 1}, kConfirmedAt);
  assert(plan.upsertedReceipts.size() == 1);
  assert(plan.upsertedReceipts.front().source ==
         ir::IrReceiptConfirmationSource::Snapshot);
  assert(plan.purgedSucceededOutboxRowIds.empty());

  local.currentReceipt = receiptFor(local, "remote-succeeded");
  plan = ir::planScoreReconciliation(kProvider, kOrigin, std::span{&local, 1},
                                     std::span{&remote, 1}, kConfirmedAt);
  assert(plan.purgedSucceededOutboxRowIds == std::vector<std::int64_t>{415});

  for (const auto state : {ir::IrOutboxState::BlockedConfiguration,
                           ir::IrOutboxState::FailedPermanent}) {
    local.outboxState = state;
    plan = ir::planScoreReconciliation(kProvider, kOrigin, std::span{&local, 1},
                                       std::span{&remote, 1}, kConfirmedAt);
    assert(plan.settledOutboxRowIds == std::vector<std::int64_t>{415});
    assert(plan.purgedSucceededOutboxRowIds.empty());
  }
}

} // namespace

int main() {
  testExactRemoteIdIsObserved();
  testEligibleLocalProofCreatesSnapshotReceipt();
  testForeignReceiptBoundaryIsNeverMutatedOrReused();
  testDisappearedSnapshotReceiptIsDeleted();
  testUnobservedSubmissionReceiptIsPreservedWhenProofIsAbsent();
  testMatchedPendingOutboxIsSettled();
  testUploadingOutboxFailsPlannerValidation();
  testAwaitingRemoteResultDoesNotBlockUnrelatedReconciliation();
  testPlanMutationsHaveDeterministicIdentityOrder();
  testPlannerRejectsInvalidIdentityTimeAndSnapshotRows();
  testProofMatchingEnforcesHashAndGameBoundaries();
  testAmbiguousAndIneligibleProofsNeverCreateReceipts();
  testUnobservedSubmissionReceiptUsesUniqueProofWithoutChangingSource();
  testIdenticalLocalProofsShareOneDeterministicRemoteIdentity();
  testOutboxRowsRequireReceiptResolutionBeforeRemoval();
  std::cout << "IR score reconciliation tests passed\n";
  return 0;
}
