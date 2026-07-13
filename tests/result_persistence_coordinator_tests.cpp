#include "../src/ResultPersistenceCoordinator.h"

#include "../src/ProfileDatabaseActivity.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The pure coordinator target intentionally does not link the database helper
// implementations. These definitions satisfy the production adapter
// constructor while every test exercises injected dependencies.
result_persistence::StageOutcome ReplayDBHelper::StageChartResult(
    const result_persistence::ChartResultAttempt &) {
  std::abort();
}

result_persistence::PendingReadOutcome
ReplayDBHelper::LoadPendingChartScore(std::string_view) {
  std::abort();
}

result_persistence::PendingBatchOutcome
ReplayDBHelper::ListPendingChartScores(std::size_t) {
  std::abort();
}

result_persistence::AcknowledgeOutcome
ReplayDBHelper::AcknowledgePendingChartScore(std::string_view, int) {
  std::abort();
}

result_persistence::RecoveryMarkOutcome
ReplayDBHelper::RecordPendingChartScoreRecoveryAttempt(
    std::string_view, result_persistence::RecoveryAttemptKind) {
  std::abort();
}

result_persistence::ProjectionOutcome ScoreDBHelper::SaveProjectedScore(
    const result_persistence::PendingChartScoreWrite &) {
  std::abort();
}

namespace {

using namespace result_persistence;

constexpr std::string_view kUnstagedMessage =
    "This result could not be stored. Retry before leaving to avoid losing "
    "it.";
constexpr std::string_view kPendingScoreMessage =
    "The replay is safe, but the score is still pending. Retry now or it will "
    "be retried automatically later.";
constexpr std::string_view kPendingAcknowledgementMessage =
    "The result was stored, but save confirmation is pending. Retrying is "
    "safe.";
constexpr std::string_view kUnstagedConflictMessage =
    "This result conflicts with an existing save and was not stored. Retry "
    "before leaving; continuing will discard this result.";
constexpr std::string_view kPendingConflictMessage =
    "This saved replay could not be verified against its score. It was kept "
    "for recovery and was not overwritten.";
constexpr std::string_view kRecoveryMessage =
    "Some previously completed results are still waiting to be saved. They "
    "were kept safely and will be retried later.";

ChartResultAttempt attempt(std::string id = "attempt-a") {
  ChartResultAttempt value;
  value.attemptId = std::move(id);
  return value;
}

StageReceipt receipt(std::string id = "attempt-a", int replayId = 17,
                     bool scorePending = true) {
  return {.attemptId = std::move(id),
          .replayId = replayId,
          .createdAt = "2026-07-14 12:34:56",
          .scorePending = scorePending};
}

PendingChartScoreWrite pending(std::string id = "attempt-a",
                               int replayId = 17) {
  return {.attemptId = std::move(id),
          .replayId = replayId,
          .createdAt = "2026-07-14 12:34:56",
          .score = {}};
}

struct Harness {
  StageOutcome stageResult{
      .status = StageStatus::Staged, .receipt = receipt(), .diagnostic = {}};
  PendingReadOutcome loadResult{
      .status = PendingReadStatus::Found, .value = pending(), .diagnostic = {}};
  PendingBatchOutcome listResult{
      .storageAvailable = true, .entries = {}, .diagnostic = {}};
  ProjectionOutcome projectResult{.status = ProjectionStatus::Inserted,
                                  .diagnostic = {}};
  AcknowledgeOutcome acknowledgeResult{
      .status = AcknowledgeStatus::Acknowledged, .diagnostic = {}};
  RecoveryMarkOutcome markResult{.status = RecoveryMarkStatus::Recorded,
                                 .diagnostic = {}};

  std::function<StageOutcome(const ChartResultAttempt &)> stageHandler;
  std::function<PendingReadOutcome(std::string_view)> loadHandler;
  std::function<PendingBatchOutcome(std::size_t)> listHandler;
  std::function<ProjectionOutcome(const PendingChartScoreWrite &)>
      projectHandler;
  std::function<AcknowledgeOutcome(std::string_view, int)> acknowledgeHandler;
  std::function<RecoveryMarkOutcome(std::string_view, RecoveryAttemptKind)>
      markHandler;

  std::vector<std::string> events;
  std::size_t stageCalls = 0;
  std::size_t loadCalls = 0;
  std::size_t listCalls = 0;
  std::size_t projectCalls = 0;
  std::size_t acknowledgeCalls = 0;
  std::size_t markCalls = 0;

  Dependencies dependencies() {
    return {
        .stage =
            [this](const ChartResultAttempt &value) {
              assert(profile_database_activity::writesActive());
              ++stageCalls;
              events.push_back("stage:" + value.attemptId);
              return stageHandler ? stageHandler(value) : stageResult;
            },
        .loadPending =
            [this](std::string_view attemptId) {
              assert(profile_database_activity::writesActive());
              ++loadCalls;
              events.push_back("load:" + std::string(attemptId));
              return loadHandler ? loadHandler(attemptId) : loadResult;
            },
        .listPending =
            [this](std::size_t limit) {
              assert(profile_database_activity::writesActive());
              ++listCalls;
              events.push_back("list:" + std::to_string(limit));
              return listHandler ? listHandler(limit) : listResult;
            },
        .project =
            [this](const PendingChartScoreWrite &value) {
              assert(profile_database_activity::writesActive());
              ++projectCalls;
              events.push_back("project:" + value.attemptId);
              return projectHandler ? projectHandler(value) : projectResult;
            },
        .acknowledge =
            [this](std::string_view attemptId, int replayId) {
              assert(profile_database_activity::writesActive());
              ++acknowledgeCalls;
              events.push_back("ack:" + std::string(attemptId) + ":" +
                               std::to_string(replayId));
              return acknowledgeHandler
                         ? acknowledgeHandler(attemptId, replayId)
                         : acknowledgeResult;
            },
        .recordRecoveryAttempt =
            [this](std::string_view attemptId, RecoveryAttemptKind kind) {
              assert(profile_database_activity::writesActive());
              ++markCalls;
              events.push_back("mark:" + std::string(attemptId) + ":" +
                               (kind == RecoveryAttemptKind::StorageFailure
                                    ? "storage"
                                    : "conflict"));
              return markHandler ? markHandler(attemptId, kind) : markResult;
            },
    };
  }
};

void assertOnlyStageCalled(const Harness &harness,
                           std::string_view attemptId = "attempt-a") {
  assert(harness.events ==
         std::vector<std::string>{"stage:" + std::string(attemptId)});
  assert(harness.stageCalls == 1);
  assert(harness.loadCalls == 0);
  assert(harness.listCalls == 0);
  assert(harness.projectCalls == 0);
  assert(harness.acknowledgeCalls == 0);
  assert(harness.markCalls == 0);
}

void testPersistOrdersStageLoadProjectAcknowledge() {
  Harness harness;
  Coordinator coordinator(harness.dependencies());

  const SaveOutcome outcome = coordinator.persist(attempt());

  assert(outcome.state == SaveState::Saved);
  assert(outcome.saved());
  assert(outcome.durable());
  assert(outcome.receipt.has_value());
  assert(outcome.receipt->replayId == 17);
  assert(outcome.userMessage.empty());
  assert(outcome.diagnostic.empty());
  assert((harness.events ==
          std::vector<std::string>{"stage:attempt-a", "load:attempt-a",
                                   "project:attempt-a", "ack:attempt-a:17"}));
  assert(harness.stageCalls == 1);
  assert(harness.loadCalls == 1);
  assert(harness.listCalls == 0);
  assert(harness.projectCalls == 1);
  assert(harness.acknowledgeCalls == 1);
  assert(harness.markCalls == 0);
}

void testStageStorageFailureReturnsTruthfulUnstagedMessage() {
  Harness harness;
  harness.stageResult = {.status = StageStatus::StorageFailure,
                         .receipt = receipt(),
                         .diagnostic = "stage database unavailable"};
  Coordinator coordinator(harness.dependencies());

  const SaveOutcome outcome = coordinator.persist(attempt());

  assert(outcome.state == SaveState::Unstaged);
  assert(!outcome.saved());
  assert(!outcome.durable());
  assert(!outcome.receipt.has_value());
  assert(outcome.userMessage == kUnstagedMessage);
  assert(outcome.diagnostic == "stage database unavailable");
  assertOnlyStageCalled(harness);
}

void testStageConflictReturnsUnstagedConflictWithoutReceipt() {
  Harness harness;
  harness.stageResult = {.status = StageStatus::IntegrityConflict,
                         .receipt = receipt(),
                         .diagnostic = "attempt fingerprint mismatch"};
  Coordinator coordinator(harness.dependencies());

  const SaveOutcome outcome = coordinator.persist(attempt());

  assert(outcome.state == SaveState::UnstagedConflict);
  assert(!outcome.saved());
  assert(!outcome.durable());
  assert(!outcome.receipt.has_value());
  assert(outcome.userMessage == kUnstagedConflictMessage);
  assert(outcome.diagnostic == "attempt fingerprint mismatch");
  assertOnlyStageCalled(harness);
}

void testMalformedSuccessfulStageMetadataIsNotDurable() {
  const auto assertRejected = [](StageOutcome stageResult) {
    Harness harness;
    harness.stageResult = std::move(stageResult);
    Coordinator coordinator(harness.dependencies());

    const SaveOutcome outcome = coordinator.persist(attempt());

    assert(outcome.state == SaveState::UnstagedConflict);
    assert(!outcome.saved());
    assert(!outcome.durable());
    assert(!outcome.receipt.has_value());
    assert(outcome.userMessage == kUnstagedConflictMessage);
    assert(outcome.diagnostic.find("inconsistent success metadata") !=
           std::string::npos);
    assertOnlyStageCalled(harness);
  };

  assertRejected({.status = StageStatus::Staged,
                  .receipt = std::nullopt,
                  .diagnostic = "missing receipt"});
  assertRejected({.status = StageStatus::AlreadyStaged,
                  .receipt = receipt("different-attempt", 17, false),
                  .diagnostic = "mismatched receipt"});
  assertRejected({.status = StageStatus::Staged,
                  .receipt = receipt("attempt-a", 17, false),
                  .diagnostic = "new stage cannot be confirmed already"});
}

void testProjectionFailureRetainsPendingScore() {
  Harness harness;
  harness.projectResult = {.status = ProjectionStatus::StorageFailure,
                           .diagnostic = "score database unavailable"};
  Coordinator coordinator(harness.dependencies());

  const SaveOutcome outcome = coordinator.persist(attempt());

  assert(outcome.state == SaveState::PendingScore);
  assert(!outcome.saved());
  assert(outcome.durable());
  assert(outcome.receipt.has_value());
  assert(outcome.userMessage == kPendingScoreMessage);
  assert(outcome.diagnostic == "score database unavailable");
  assert((harness.events == std::vector<std::string>{"stage:attempt-a",
                                                     "load:attempt-a",
                                                     "project:attempt-a"}));
  assert(harness.stageCalls == 1);
  assert(harness.loadCalls == 1);
  assert(harness.listCalls == 0);
  assert(harness.projectCalls == 1);
  assert(harness.acknowledgeCalls == 0);
  assert(harness.markCalls == 0);
}

void testProjectionConflictReturnsDurablePendingConflict() {
  Harness harness;
  harness.projectResult = {.status = ProjectionStatus::IntegrityConflict,
                           .diagnostic = "score payload differs"};
  Coordinator coordinator(harness.dependencies());

  const SaveOutcome outcome = coordinator.persist(attempt());

  assert(outcome.state == SaveState::PendingConflict);
  assert(!outcome.saved());
  assert(outcome.durable());
  assert(outcome.receipt.has_value());
  assert(outcome.userMessage == kPendingConflictMessage);
  assert(outcome.diagnostic == "score payload differs");
  assert((harness.events == std::vector<std::string>{"stage:attempt-a",
                                                     "load:attempt-a",
                                                     "project:attempt-a"}));
  assert(harness.stageCalls == 1);
  assert(harness.loadCalls == 1);
  assert(harness.listCalls == 0);
  assert(harness.projectCalls == 1);
  assert(harness.acknowledgeCalls == 0);
  assert(harness.markCalls == 0);
}

void testAcknowledgeFailureIsDurableAndRetryable() {
  Harness harness;
  harness.acknowledgeResult = {.status = AcknowledgeStatus::StorageFailure,
                               .diagnostic = "acknowledgement write failed"};
  Coordinator coordinator(harness.dependencies());

  const SaveOutcome outcome = coordinator.persist(attempt());

  assert(outcome.state == SaveState::PendingAcknowledgement);
  assert(!outcome.saved());
  assert(outcome.durable());
  assert(outcome.receipt.has_value());
  assert(outcome.userMessage == kPendingAcknowledgementMessage);
  assert(outcome.diagnostic == "acknowledgement write failed");
  assert((harness.events ==
          std::vector<std::string>{"stage:attempt-a", "load:attempt-a",
                                   "project:attempt-a", "ack:attempt-a:17"}));
  assert(harness.stageCalls == 1);
  assert(harness.loadCalls == 1);
  assert(harness.listCalls == 0);
  assert(harness.projectCalls == 1);
  assert(harness.acknowledgeCalls == 1);
  assert(harness.markCalls == 0);
}

void testAcknowledgeConflictRetainsPendingConflict() {
  Harness harness;
  harness.acknowledgeResult = {.status = AcknowledgeStatus::IntegrityConflict,
                               .diagnostic = "outbox replay identity differs"};
  Coordinator coordinator(harness.dependencies());

  const SaveOutcome outcome = coordinator.persist(attempt());

  assert(outcome.state == SaveState::PendingConflict);
  assert(!outcome.saved());
  assert(outcome.durable());
  assert(outcome.receipt.has_value());
  assert(outcome.userMessage == kPendingConflictMessage);
  assert(outcome.diagnostic == "outbox replay identity differs");
  assert((harness.events ==
          std::vector<std::string>{"stage:attempt-a", "load:attempt-a",
                                   "project:attempt-a", "ack:attempt-a:17"}));
  assert(harness.stageCalls == 1);
  assert(harness.loadCalls == 1);
  assert(harness.listCalls == 0);
  assert(harness.projectCalls == 1);
  assert(harness.acknowledgeCalls == 1);
  assert(harness.markCalls == 0);
}

void testRetrySkipsAlreadyConfirmedReplayAndScore() {
  Harness harness;
  harness.stageResult = {.status = StageStatus::AlreadyStaged,
                         .receipt = receipt("attempt-a", 17, false),
                         .diagnostic = {}};
  Coordinator coordinator(harness.dependencies());

  const SaveOutcome outcome = coordinator.persist(attempt());

  assert(outcome.state == SaveState::Saved);
  assert(outcome.saved());
  assert(outcome.durable());
  assert(outcome.receipt.has_value());
  assert(!outcome.receipt->scorePending);
  assert(outcome.userMessage.empty());
  assert(outcome.diagnostic.empty());
  assertOnlyStageCalled(harness);
}

void testRecoveryContinuesAfterMalformedAndFailedRows() {
  Harness harness;
  harness.listResult = {.storageAvailable = true,
                        .entries =
                            {
                                {.status = PendingReadStatus::IntegrityConflict,
                                 .attemptId = "malformed",
                                 .value = std::nullopt,
                                 .diagnostic = "malformed pending row"},
                                {.status = PendingReadStatus::Found,
                                 .attemptId = "storage",
                                 .value = pending("storage", 18),
                                 .diagnostic = {}},
                                {.status = PendingReadStatus::Found,
                                 .attemptId = "saved",
                                 .value = pending("saved", 19),
                                 .diagnostic = {}},
                                {.status = PendingReadStatus::Found,
                                 .attemptId = "ack-conflict",
                                 .value = pending("ack-conflict", 20),
                                 .diagnostic = {}},
                            },
                        .diagnostic = {}};
  harness.projectHandler = [](const PendingChartScoreWrite &value) {
    if (value.attemptId == "storage") {
      return ProjectionOutcome{.status = ProjectionStatus::StorageFailure,
                               .diagnostic = "project unavailable"};
    }
    return ProjectionOutcome{.status = ProjectionStatus::Inserted,
                             .diagnostic = {}};
  };
  harness.acknowledgeHandler = [](std::string_view attemptId, int) {
    if (attemptId == "ack-conflict") {
      return AcknowledgeOutcome{.status = AcknowledgeStatus::IntegrityConflict,
                                .diagnostic = "ack conflict"};
    }
    return AcknowledgeOutcome{.status = AcknowledgeStatus::Acknowledged,
                              .diagnostic = {}};
  };
  Coordinator coordinator(harness.dependencies());

  const RecoverySummary summary = coordinator.recoverAll();

  assert(summary.attempted == 4);
  assert(summary.saved == 1);
  assert(summary.pending == 1);
  assert(summary.conflicts == 2);
  assert(summary.userMessage == kRecoveryMessage);
  assert(summary.diagnostic.find("malformed pending row") != std::string::npos);
  assert(summary.diagnostic.find("project unavailable") != std::string::npos);
  assert(summary.diagnostic.find("ack conflict") != std::string::npos);
  assert((harness.events == std::vector<std::string>{
                                "list:256", "mark:malformed:conflict",
                                "project:storage", "mark:storage:storage",
                                "project:saved", "ack:saved:19",
                                "project:ack-conflict", "ack:ack-conflict:20",
                                "mark:ack-conflict:conflict"}));
  assert(harness.stageCalls == 0);
  assert(harness.loadCalls == 0);
  assert(harness.listCalls == 1);
  assert(harness.projectCalls == 3);
  assert(harness.acknowledgeCalls == 2);
  assert(harness.markCalls == 3);
}

void testMessagesNeverContainInjectedDiagnostics() {
  constexpr std::string_view injected =
      "attempt-a /private/profile/score.db SELECT secret FROM scores";

  const auto assertHidden = [&](SaveOutcome outcome, const Harness &harness,
                                std::vector<std::string> expectedEvents) {
    assert(outcome.userMessage.find(injected) == std::string::npos);
    assert(outcome.diagnostic.find(injected) != std::string::npos);
    assert(harness.events == expectedEvents);
  };

  {
    Harness harness;
    harness.stageResult = {.status = StageStatus::StorageFailure,
                           .receipt = std::nullopt,
                           .diagnostic = std::string(injected)};
    Coordinator coordinator(harness.dependencies());
    assertHidden(coordinator.persist(attempt()), harness, {"stage:attempt-a"});
  }
  {
    Harness harness;
    harness.loadResult = {.status = PendingReadStatus::StorageFailure,
                          .value = std::nullopt,
                          .diagnostic = std::string(injected)};
    Coordinator coordinator(harness.dependencies());
    assertHidden(coordinator.persist(attempt()), harness,
                 {"stage:attempt-a", "load:attempt-a"});
  }
  {
    Harness harness;
    harness.projectResult = {.status = ProjectionStatus::StorageFailure,
                             .diagnostic = std::string(injected)};
    Coordinator coordinator(harness.dependencies());
    assertHidden(coordinator.persist(attempt()), harness,
                 {"stage:attempt-a", "load:attempt-a", "project:attempt-a"});
  }
  {
    Harness harness;
    harness.acknowledgeResult = {.status = AcknowledgeStatus::StorageFailure,
                                 .diagnostic = std::string(injected)};
    Coordinator coordinator(harness.dependencies());
    assertHidden(coordinator.persist(attempt()), harness,
                 {"stage:attempt-a", "load:attempt-a", "project:attempt-a",
                  "ack:attempt-a:17"});
  }
  {
    Harness harness;
    harness.listResult = {.storageAvailable = false,
                          .entries = {},
                          .diagnostic = std::string(injected)};
    Coordinator coordinator(harness.dependencies());
    const RecoverySummary summary = coordinator.recoverAll();
    assert(summary.attempted == 0);
    assert(summary.saved == 0);
    assert(summary.pending == 1);
    assert(summary.conflicts == 0);
    assert(summary.userMessage == kRecoveryMessage);
    assert(summary.userMessage.find(injected) == std::string::npos);
    assert(summary.diagnostic.find(injected) != std::string::npos);
    assert(harness.events == std::vector<std::string>{"list:256"});
    assert(harness.listCalls == 1);
    assert(harness.projectCalls == 0);
    assert(harness.acknowledgeCalls == 0);
    assert(harness.markCalls == 0);
  }
}

void testRecoveryLimitIsExactly256() {
  Harness harness;
  std::vector<std::size_t> limits;
  harness.listHandler = [&](std::size_t limit) {
    limits.push_back(limit);
    return PendingBatchOutcome{
        .storageAvailable = true, .entries = {}, .diagnostic = {}};
  };
  Coordinator coordinator(harness.dependencies());

  const RecoverySummary capped = coordinator.recoverAll(999);
  const RecoverySummary exact = coordinator.recoverAll(7);
  const RecoverySummary empty = coordinator.recoverAll(0);

  assert(capped.attempted == 0 && capped.userMessage.empty());
  assert(exact.attempted == 0 && exact.userMessage.empty());
  assert(empty.attempted == 0 && empty.userMessage.empty());
  assert((limits == std::vector<std::size_t>{256, 7, 0}));
  assert((harness.events ==
          std::vector<std::string>{"list:256", "list:7", "list:0"}));
  assert(harness.listCalls == 3);
  assert(harness.projectCalls == 0);
  assert(harness.acknowledgeCalls == 0);
  assert(harness.markCalls == 0);
}

void testPersistentFirstBatchDoesNotStarveNewValidRow() {
  Harness harness;
  std::set<std::string> marked;
  std::size_t batchNumber = 0;
  harness.listHandler = [&](std::size_t limit) {
    assert(limit == 256);
    ++batchNumber;
    PendingBatchOutcome outcome{
        .storageAvailable = true, .entries = {}, .diagnostic = {}};
    if (batchNumber == 1) {
      for (std::size_t index = 0; index < 256; ++index) {
        const std::string id = "conflict-" + std::to_string(index);
        outcome.entries.push_back(
            {.status = PendingReadStatus::IntegrityConflict,
             .attemptId = id,
             .value = std::nullopt,
             .diagnostic = "persistent conflict"});
      }
    } else {
      assert(marked.size() == 256);
      outcome.entries.push_back({.status = PendingReadStatus::Found,
                                 .attemptId = "new-valid",
                                 .value = pending("new-valid", 99),
                                 .diagnostic = {}});
    }
    return outcome;
  };
  harness.markHandler = [&](std::string_view attemptId,
                            RecoveryAttemptKind kind) {
    assert(kind == RecoveryAttemptKind::IntegrityConflict);
    marked.insert(std::string(attemptId));
    return RecoveryMarkOutcome{.status = RecoveryMarkStatus::Recorded,
                               .diagnostic = {}};
  };
  Coordinator coordinator(harness.dependencies());

  const RecoverySummary first = coordinator.recoverAll();
  const RecoverySummary second = coordinator.recoverAll();

  assert(first.attempted == 256);
  assert(first.saved == 0);
  assert(first.pending == 0);
  assert(first.conflicts == 256);
  assert(first.userMessage == kRecoveryMessage);
  assert(second.attempted == 1);
  assert(second.saved == 1);
  assert(second.pending == 0);
  assert(second.conflicts == 0);
  assert(second.userMessage.empty());
  std::vector<std::string> expected{"list:256"};
  for (std::size_t index = 0; index < 256; ++index) {
    expected.push_back("mark:conflict-" + std::to_string(index) + ":conflict");
  }
  expected.insert(expected.end(),
                  {"list:256", "project:new-valid", "ack:new-valid:99"});
  assert(harness.events == expected);
  assert(harness.listCalls == 2);
  assert(harness.projectCalls == 1);
  assert(harness.acknowledgeCalls == 1);
  assert(harness.markCalls == 256);
}

} // namespace

int main() {
  testPersistOrdersStageLoadProjectAcknowledge();
  testStageStorageFailureReturnsTruthfulUnstagedMessage();
  testStageConflictReturnsUnstagedConflictWithoutReceipt();
  testMalformedSuccessfulStageMetadataIsNotDurable();
  testProjectionFailureRetainsPendingScore();
  testProjectionConflictReturnsDurablePendingConflict();
  testAcknowledgeFailureIsDurableAndRetryable();
  testAcknowledgeConflictRetainsPendingConflict();
  testRetrySkipsAlreadyConfirmedReplayAndScore();
  testRecoveryContinuesAfterMalformedAndFailedRows();
  testMessagesNeverContainInjectedDiagnostics();
  testRecoveryLimitIsExactly256();
  testPersistentFirstBatchDoesNotStarveNewValidRow();
  std::cout << "result persistence coordinator tests passed\n";
  return 0;
}
