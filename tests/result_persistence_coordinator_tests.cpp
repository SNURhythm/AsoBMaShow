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
constexpr std::string_view kInvalidAttemptMessage =
    "This result could not be prepared for saving and cannot be retried. "
    "Continuing will discard it.";
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

void testSaveOutcomePresentationSemantics() {
  const ChartResultAttempt currentAttempt = attempt();
  const SaveOutcome invalid{
      .state = SaveState::InvalidAttempt,
      .receipt = std::nullopt,
      .userMessage = std::string(saveStateUserMessage(SaveState::InvalidAttempt)),
      .diagnostic = "deterministic validation failure",
  };
  assert(!invalid.saved());
  assert(!invalid.durable());
  assert(!invalid.retryable());
  assert(invalid.userMessage == kInvalidAttemptMessage);
  assert(invalid.validatedReceiptFor(currentAttempt) == nullptr);
  assert(invalid.requiresUserDecision(false, false));
  assert(!invalid.requiresUserDecision(false, true));

  const SaveOutcome unstaged{
      .state = SaveState::Unstaged,
      .receipt = std::nullopt,
      .userMessage = std::string(kUnstagedMessage),
      .diagnostic = {},
  };
  assert(unstaged.retryable());
  assert(unstaged.requiresUserDecision(true, false));

  const SaveOutcome saved{
      .state = SaveState::Saved,
      .receipt = receipt("attempt-a", 17, false),
      .userMessage = {},
      .diagnostic = {},
  };
  assert(!saved.retryable());
  assert(!saved.requiresUserDecision(true, false));
  assert(saved.validatedReceiptFor(currentAttempt) == &*saved.receipt);

  SaveOutcome mismatched = saved;
  mismatched.receipt->attemptId = "different-attempt";
  assert(mismatched.validatedReceiptFor(currentAttempt) == nullptr);
  mismatched = saved;
  mismatched.receipt->replayId = 0;
  assert(mismatched.validatedReceiptFor(currentAttempt) == nullptr);
  mismatched = saved;
  mismatched.receipt->createdAt.clear();
  assert(mismatched.validatedReceiptFor(currentAttempt) == nullptr);

  SaveOutcome nondurable = saved;
  nondurable.state = SaveState::Unstaged;
  assert(nondurable.validatedReceiptFor(currentAttempt) == nullptr);

  const SaveOutcome empty;
  assert(!empty.requiresUserDecision(false, false));
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
  assertRejected({.status = StageStatus::AlreadyStaged,
                  .receipt = receipt("attempt-a", 0, true),
                  .diagnostic = "non-positive replay id"});
  assertRejected({.status = StageStatus::AlreadyStaged,
                  .receipt = receipt("attempt-a", -1, true),
                  .diagnostic = "negative replay id"});
  StageReceipt missingTimestamp = receipt();
  missingTimestamp.createdAt.clear();
  assertRejected({.status = StageStatus::AlreadyStaged,
                  .receipt = std::move(missingTimestamp),
                  .diagnostic = "missing result timestamp"});
}

void testPendingReadFailuresStopBeforeProjection() {
  const auto assertReadFailure = [](PendingReadOutcome readResult,
                                    SaveState expectedState,
                                    std::string_view expectedMessage,
                                    std::string_view expectedDiagnostic) {
    Harness harness;
    harness.loadResult = std::move(readResult);
    Coordinator coordinator(harness.dependencies());

    const SaveOutcome outcome = coordinator.persist(attempt());

    assert(outcome.state == expectedState);
    assert(!outcome.saved());
    assert(outcome.durable());
    assert(outcome.receipt.has_value());
    assert(outcome.receipt->attemptId == "attempt-a");
    assert(outcome.receipt->replayId == 17);
    assert(outcome.userMessage == expectedMessage);
    assert(outcome.diagnostic == expectedDiagnostic);
    assert((harness.events ==
            std::vector<std::string>{"stage:attempt-a", "load:attempt-a"}));
    assert(harness.stageCalls == 1);
    assert(harness.loadCalls == 1);
    assert(harness.listCalls == 0);
    assert(harness.projectCalls == 0);
    assert(harness.acknowledgeCalls == 0);
    assert(harness.markCalls == 0);
  };

  assertReadFailure({.status = PendingReadStatus::StorageFailure,
                     .value = std::nullopt,
                     .diagnostic = "pending read unavailable"},
                    SaveState::PendingScore, kPendingScoreMessage,
                    "pending read unavailable");
  assertReadFailure({.status = PendingReadStatus::NotFound,
                     .value = std::nullopt,
                     .diagnostic = "pending row missing"},
                    SaveState::PendingConflict, kPendingConflictMessage,
                    "pending row missing");
  assertReadFailure({.status = PendingReadStatus::IntegrityConflict,
                     .value = std::nullopt,
                     .diagnostic = "pending row corrupt"},
                    SaveState::PendingConflict, kPendingConflictMessage,
                    "pending row corrupt");
  assertReadFailure({.status = PendingReadStatus::Found,
                     .value = std::nullopt,
                     .diagnostic = {}},
                    SaveState::PendingConflict, kPendingConflictMessage,
                    "pending read returned Found without a payload");
}

void testPendingPayloadMismatchStopsBeforeProjection() {
  const auto assertMismatch = [](PendingChartScoreWrite value,
                                 std::string_view expectedDiagnostic) {
    Harness harness;
    harness.loadResult = {.status = PendingReadStatus::Found,
                          .value = std::move(value),
                          .diagnostic = {}};
    Coordinator coordinator(harness.dependencies());

    const SaveOutcome outcome = coordinator.persist(attempt());

    assert(outcome.state == SaveState::PendingConflict);
    assert(!outcome.saved());
    assert(outcome.durable());
    assert(outcome.receipt.has_value());
    assert(outcome.receipt->attemptId == "attempt-a");
    assert(outcome.receipt->replayId == 17);
    assert(outcome.userMessage == kPendingConflictMessage);
    assert(outcome.diagnostic == expectedDiagnostic);
    assert((harness.events ==
            std::vector<std::string>{"stage:attempt-a", "load:attempt-a"}));
    assert(harness.stageCalls == 1);
    assert(harness.loadCalls == 1);
    assert(harness.projectCalls == 0);
    assert(harness.acknowledgeCalls == 0);
  };

  assertMismatch(pending("different-attempt", 17),
                 "pending score identity does not match its receipt");
  assertMismatch(pending("attempt-a", 18),
                 "pending score identity does not match its receipt");
  PendingChartScoreWrite timestampMismatch = pending();
  timestampMismatch.createdAt = "2026-07-14 12:34:57";
  assertMismatch(std::move(timestampMismatch),
                 "pending score timestamp does not match its receipt");
  PendingChartScoreWrite scoreMismatch = pending();
  ++scoreMismatch.score.fast;
  assertMismatch(std::move(scoreMismatch),
                 "pending score payload does not match the current attempt");
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

void testRetryAfterAcknowledgementFailureResumesIdempotently() {
  Harness harness;
  std::size_t stageNumber = 0;
  std::size_t projectNumber = 0;
  std::size_t acknowledgeNumber = 0;
  harness.stageHandler = [&](const ChartResultAttempt &) {
    ++stageNumber;
    return StageOutcome{.status = stageNumber == 1 ? StageStatus::Staged
                                                   : StageStatus::AlreadyStaged,
                        .receipt = receipt(),
                        .diagnostic = {}};
  };
  harness.projectHandler = [&](const PendingChartScoreWrite &) {
    ++projectNumber;
    return ProjectionOutcome{.status = projectNumber == 1
                                           ? ProjectionStatus::Inserted
                                           : ProjectionStatus::AlreadyPresent,
                             .diagnostic = {}};
  };
  harness.acknowledgeHandler = [&](std::string_view, int) {
    ++acknowledgeNumber;
    if (acknowledgeNumber == 1) {
      return AcknowledgeOutcome{.status = AcknowledgeStatus::StorageFailure,
                                .diagnostic = "acknowledgement unavailable"};
    }
    return AcknowledgeOutcome{.status = AcknowledgeStatus::Acknowledged,
                              .diagnostic = {}};
  };
  Coordinator coordinator(harness.dependencies());
  const ChartResultAttempt fixedAttempt = attempt();

  const SaveOutcome first = coordinator.persist(fixedAttempt);

  assert(first.state == SaveState::PendingAcknowledgement);
  assert(!first.saved());
  assert(first.durable());
  assert(first.receipt.has_value());
  assert(first.receipt->attemptId == fixedAttempt.attemptId);
  assert(first.receipt->replayId == 17);
  assert(first.receipt->scorePending);
  assert(first.userMessage == kPendingAcknowledgementMessage);
  assert(first.diagnostic == "acknowledgement unavailable");
  assert((harness.events ==
          std::vector<std::string>{"stage:attempt-a", "load:attempt-a",
                                   "project:attempt-a", "ack:attempt-a:17"}));
  assert(harness.stageCalls == 1);
  assert(harness.loadCalls == 1);
  assert(harness.projectCalls == 1);
  assert(harness.acknowledgeCalls == 1);

  const SaveOutcome second = coordinator.persist(fixedAttempt);

  assert(second.state == SaveState::Saved);
  assert(second.saved());
  assert(second.durable());
  assert(second.receipt.has_value());
  assert(second.receipt->attemptId == fixedAttempt.attemptId);
  assert(second.receipt->replayId == 17);
  assert(!second.receipt->scorePending);
  assert(second.userMessage.empty());
  assert(second.diagnostic.empty());
  assert((harness.events ==
          std::vector<std::string>{"stage:attempt-a", "load:attempt-a",
                                   "project:attempt-a", "ack:attempt-a:17",
                                   "stage:attempt-a", "load:attempt-a",
                                   "project:attempt-a", "ack:attempt-a:17"}));
  assert(harness.stageCalls == 2);
  assert(harness.loadCalls == 2);
  assert(harness.listCalls == 0);
  assert(harness.projectCalls == 2);
  assert(harness.acknowledgeCalls == 2);
  assert(harness.markCalls == 0);
}

void testAlreadyAcknowledgedIsIdempotentPersistSuccess() {
  Harness harness;
  harness.acknowledgeResult = {.status = AcknowledgeStatus::AlreadyAcknowledged,
                               .diagnostic = {}};
  Coordinator coordinator(harness.dependencies());

  const SaveOutcome outcome = coordinator.persist(attempt());

  assert(outcome.state == SaveState::Saved);
  assert(outcome.saved());
  assert(outcome.durable());
  assert(outcome.receipt.has_value());
  assert(outcome.receipt->attemptId == "attempt-a");
  assert(outcome.receipt->replayId == 17);
  assert(!outcome.receipt->scorePending);
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

void testUnknownPersistStatusesFailClosed() {
  {
    Harness harness;
    harness.stageResult = {.status = static_cast<StageStatus>(999),
                           .receipt = receipt(),
                           .diagnostic = {}};
    Coordinator coordinator(harness.dependencies());

    const SaveOutcome outcome = coordinator.persist(attempt());

    assert(outcome.state == SaveState::UnstagedConflict);
    assert(!outcome.saved());
    assert(!outcome.durable());
    assert(!outcome.receipt.has_value());
    assert(outcome.userMessage == kUnstagedConflictMessage);
    assert(outcome.diagnostic == "unknown staging status");
    assertOnlyStageCalled(harness);
  }
  {
    Harness harness;
    harness.loadResult = {.status = static_cast<PendingReadStatus>(999),
                          .value = pending(),
                          .diagnostic = {}};
    Coordinator coordinator(harness.dependencies());

    const SaveOutcome outcome = coordinator.persist(attempt());

    assert(outcome.state == SaveState::PendingConflict);
    assert(!outcome.saved());
    assert(outcome.durable());
    assert(outcome.receipt.has_value());
    assert(outcome.userMessage == kPendingConflictMessage);
    assert(outcome.diagnostic == "unknown pending read status");
    assert((harness.events ==
            std::vector<std::string>{"stage:attempt-a", "load:attempt-a"}));
    assert(harness.projectCalls == 0);
    assert(harness.acknowledgeCalls == 0);
  }
  {
    Harness harness;
    harness.projectResult = {.status = static_cast<ProjectionStatus>(999),
                             .diagnostic = {}};
    Coordinator coordinator(harness.dependencies());

    const SaveOutcome outcome = coordinator.persist(attempt());

    assert(outcome.state == SaveState::PendingConflict);
    assert(!outcome.saved());
    assert(outcome.durable());
    assert(outcome.receipt.has_value());
    assert(outcome.userMessage == kPendingConflictMessage);
    assert(outcome.diagnostic == "unknown projection status");
    assert((harness.events == std::vector<std::string>{"stage:attempt-a",
                                                       "load:attempt-a",
                                                       "project:attempt-a"}));
    assert(harness.projectCalls == 1);
    assert(harness.acknowledgeCalls == 0);
  }
  {
    Harness harness;
    harness.acknowledgeResult = {.status = static_cast<AcknowledgeStatus>(999),
                                 .diagnostic = {}};
    Coordinator coordinator(harness.dependencies());

    const SaveOutcome outcome = coordinator.persist(attempt());

    assert(outcome.state == SaveState::PendingConflict);
    assert(!outcome.saved());
    assert(outcome.durable());
    assert(outcome.receipt.has_value());
    assert(outcome.userMessage == kPendingConflictMessage);
    assert(outcome.diagnostic == "unknown acknowledgement status");
    assert((harness.events ==
            std::vector<std::string>{"stage:attempt-a", "load:attempt-a",
                                     "project:attempt-a", "ack:attempt-a:17"}));
    assert(harness.acknowledgeCalls == 1);
  }
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

void testRecoveryMarkFailuresRemainVisibleAndContinue() {
  Harness harness;
  harness.listResult = {
      .storageAvailable = true,
      .entries =
          {
              {.status = PendingReadStatus::Found,
               .attemptId = "mark-not-found",
               .value = pending("mark-not-found", 31),
               .diagnostic = {}},
              {.status = PendingReadStatus::Found,
               .attemptId = "mark-storage",
               .value = pending("mark-storage", 32),
               .diagnostic = {}},
              {.status = PendingReadStatus::Found,
               .attemptId = "mark-unknown",
               .value = pending("mark-unknown", 33),
               .diagnostic = {}},
              {.status = PendingReadStatus::Found,
               .attemptId = "saved-after-mark-failures",
               .value = pending("saved-after-mark-failures", 34),
               .diagnostic = {}},
          },
      .diagnostic = {}};
  harness.projectHandler = [](const PendingChartScoreWrite &value) {
    if (value.attemptId == "saved-after-mark-failures") {
      return ProjectionOutcome{.status = ProjectionStatus::Inserted,
                               .diagnostic = {}};
    }
    return ProjectionOutcome{.status = ProjectionStatus::StorageFailure,
                             .diagnostic =
                                 "projection failed for " + value.attemptId};
  };
  harness.markHandler = [](std::string_view attemptId, RecoveryAttemptKind) {
    if (attemptId == "mark-not-found") {
      return RecoveryMarkOutcome{.status = RecoveryMarkStatus::NotFound,
                                 .diagnostic = "marker row missing"};
    }
    if (attemptId == "mark-storage") {
      return RecoveryMarkOutcome{.status = RecoveryMarkStatus::StorageFailure,
                                 .diagnostic = "marker write failed"};
    }
    return RecoveryMarkOutcome{.status = static_cast<RecoveryMarkStatus>(999),
                               .diagnostic = "future marker status"};
  };
  Coordinator coordinator(harness.dependencies());

  const RecoverySummary summary = coordinator.recoverAll();

  assert(summary.attempted == 4);
  assert(summary.saved == 1);
  assert(summary.pending == 1);
  assert(summary.conflicts == 2);
  assert(summary.userMessage == kRecoveryMessage);
  assert(summary.diagnostic == "projection failed for mark-not-found\n"
                               "marker row missing\n"
                               "recovery marker did not find the pending row\n"
                               "projection failed for mark-storage\n"
                               "marker write failed\n"
                               "recovery marker could not be recorded\n"
                               "projection failed for mark-unknown\n"
                               "future marker status\n"
                               "unknown recovery marker status");
  assert((harness.events ==
          std::vector<std::string>{
              "list:256", "project:mark-not-found",
              "mark:mark-not-found:storage", "project:mark-storage",
              "mark:mark-storage:storage", "project:mark-unknown",
              "mark:mark-unknown:storage", "project:saved-after-mark-failures",
              "ack:saved-after-mark-failures:34"}));
  assert(harness.listCalls == 1);
  assert(harness.projectCalls == 4);
  assert(harness.acknowledgeCalls == 1);
  assert(harness.markCalls == 3);
}

void testUnmarkedRecoveryRowIsRetriedOnNextCall() {
  Harness harness;
  std::size_t listNumber = 0;
  harness.listHandler = [&](std::size_t limit) {
    assert(limit == 256);
    ++listNumber;
    const std::string savedId = listNumber == 1
                                    ? "saved-after-first-marker-failure"
                                    : "saved-after-second-marker-failure";
    return PendingBatchOutcome{
        .storageAvailable = true,
        .entries =
            {
                {.status = PendingReadStatus::Found,
                 .attemptId = "unmarked-pending",
                 .value = pending("unmarked-pending", 51),
                 .diagnostic = {}},
                {.status = PendingReadStatus::Found,
                 .attemptId = savedId,
                 .value = pending(savedId, listNumber == 1 ? 52 : 53),
                 .diagnostic = {}},
            },
        .diagnostic = {}};
  };
  harness.projectHandler = [](const PendingChartScoreWrite &value) {
    return ProjectionOutcome{.status = value.attemptId == "unmarked-pending"
                                           ? ProjectionStatus::StorageFailure
                                           : ProjectionStatus::Inserted,
                             .diagnostic = value.attemptId == "unmarked-pending"
                                               ? "projection still unavailable"
                                               : std::string{}};
  };
  harness.markHandler = [](std::string_view attemptId, RecoveryAttemptKind) {
    assert(attemptId == "unmarked-pending");
    return RecoveryMarkOutcome{.status = RecoveryMarkStatus::StorageFailure,
                               .diagnostic = "marker still unavailable"};
  };
  Coordinator coordinator(harness.dependencies());

  const RecoverySummary first = coordinator.recoverAll();
  const RecoverySummary second = coordinator.recoverAll();

  for (const RecoverySummary *summary : {&first, &second}) {
    assert(summary->attempted == 2);
    assert(summary->saved == 1);
    assert(summary->pending == 1);
    assert(summary->conflicts == 0);
    assert(summary->userMessage == kRecoveryMessage);
    assert(summary->diagnostic ==
           "projection still unavailable\nmarker still unavailable\n"
           "recovery marker could not be recorded");
  }
  assert((harness.events == std::vector<std::string>{
                                "list:256", "project:unmarked-pending",
                                "mark:unmarked-pending:storage",
                                "project:saved-after-first-marker-failure",
                                "ack:saved-after-first-marker-failure:52",
                                "list:256", "project:unmarked-pending",
                                "mark:unmarked-pending:storage",
                                "project:saved-after-second-marker-failure",
                                "ack:saved-after-second-marker-failure:53"}));
  assert(harness.listCalls == 2);
  assert(harness.projectCalls == 4);
  assert(harness.acknowledgeCalls == 2);
  assert(harness.markCalls == 2);
}

void testInvalidRecoveryPayloadStopsBeforeProjection() {
  const auto assertInvalid = [](PendingBatchEntry entry,
                                std::string_view expectedDiagnostic) {
    Harness harness;
    harness.listResult = {.storageAvailable = true,
                          .entries = {std::move(entry)},
                          .diagnostic = {}};
    Coordinator coordinator(harness.dependencies());

    const RecoverySummary summary = coordinator.recoverAll();

    assert(summary.attempted == 1);
    assert(summary.saved == 0);
    assert(summary.pending == 0);
    assert(summary.conflicts == 1);
    assert(summary.userMessage == kRecoveryMessage);
    assert(summary.diagnostic == expectedDiagnostic);
    assert((harness.events == std::vector<std::string>{
                                  "list:256", "mark:recovery-row:conflict"}));
    assert(harness.listCalls == 1);
    assert(harness.projectCalls == 0);
    assert(harness.acknowledgeCalls == 0);
    assert(harness.markCalls == 1);
  };

  assertInvalid({.status = PendingReadStatus::Found,
                 .attemptId = "recovery-row",
                 .value = pending("different-attempt", 61),
                 .diagnostic = {}},
                "pending recovery entry identity does not match its payload");
  assertInvalid({.status = PendingReadStatus::Found,
                 .attemptId = "recovery-row",
                 .value = pending("recovery-row", 0),
                 .diagnostic = {}},
                "pending recovery payload has invalid replay metadata");
  assertInvalid({.status = PendingReadStatus::Found,
                 .attemptId = "recovery-row",
                 .value = pending("recovery-row", -1),
                 .diagnostic = {}},
                "pending recovery payload has invalid replay metadata");
  PendingChartScoreWrite missingTimestamp = pending("recovery-row", 62);
  missingTimestamp.createdAt.clear();
  assertInvalid({.status = PendingReadStatus::Found,
                 .attemptId = "recovery-row",
                 .value = std::move(missingTimestamp),
                 .diagnostic = {}},
                "pending recovery payload has invalid replay metadata");
}

void testAlreadyAcknowledgedIsIdempotentRecoverySuccess() {
  Harness harness;
  harness.listResult = {
      .storageAvailable = true,
      .entries = {{.status = PendingReadStatus::Found,
                   .attemptId = "already-acknowledged",
                   .value = pending("already-acknowledged", 71),
                   .diagnostic = {}}},
      .diagnostic = {}};
  harness.acknowledgeResult = {.status = AcknowledgeStatus::AlreadyAcknowledged,
                               .diagnostic = {}};
  Coordinator coordinator(harness.dependencies());

  const RecoverySummary summary = coordinator.recoverAll();

  assert(summary.attempted == 1);
  assert(summary.saved == 1);
  assert(summary.pending == 0);
  assert(summary.conflicts == 0);
  assert(summary.userMessage.empty());
  assert(summary.diagnostic.empty());
  assert((harness.events ==
          std::vector<std::string>{"list:256", "project:already-acknowledged",
                                   "ack:already-acknowledged:71"}));
  assert(harness.stageCalls == 0);
  assert(harness.loadCalls == 0);
  assert(harness.listCalls == 1);
  assert(harness.projectCalls == 1);
  assert(harness.acknowledgeCalls == 1);
  assert(harness.markCalls == 0);
}

void testUnknownRecoveryStatusesFailClosed() {
  Harness harness;
  harness.listResult = {.storageAvailable = true,
                        .entries =
                            {
                                {.status = static_cast<PendingReadStatus>(999),
                                 .attemptId = "future-read",
                                 .value = std::nullopt,
                                 .diagnostic = {}},
                                {.status = PendingReadStatus::Found,
                                 .attemptId = "future-project",
                                 .value = pending("future-project", 41),
                                 .diagnostic = {}},
                                {.status = PendingReadStatus::Found,
                                 .attemptId = "future-ack",
                                 .value = pending("future-ack", 42),
                                 .diagnostic = {}},
                            },
                        .diagnostic = {}};
  harness.projectHandler = [](const PendingChartScoreWrite &value) {
    return ProjectionOutcome{.status = value.attemptId == "future-project"
                                           ? static_cast<ProjectionStatus>(999)
                                           : ProjectionStatus::AlreadyPresent,
                             .diagnostic = {}};
  };
  harness.acknowledgeHandler = [](std::string_view, int) {
    return AcknowledgeOutcome{.status = static_cast<AcknowledgeStatus>(999),
                              .diagnostic = {}};
  };
  Coordinator coordinator(harness.dependencies());

  const RecoverySummary summary = coordinator.recoverAll();

  assert(summary.attempted == 3);
  assert(summary.saved == 0);
  assert(summary.pending == 0);
  assert(summary.conflicts == 3);
  assert(summary.userMessage == kRecoveryMessage);
  assert(summary.diagnostic ==
         "unknown pending read status\nunknown projection status\n"
         "unknown acknowledgement status");
  assert((harness.events ==
          std::vector<std::string>{
              "list:256", "mark:future-read:conflict", "project:future-project",
              "mark:future-project:conflict", "project:future-ack",
              "ack:future-ack:42", "mark:future-ack:conflict"}));
  assert(harness.listCalls == 1);
  assert(harness.projectCalls == 2);
  assert(harness.acknowledgeCalls == 1);
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

void testRecoveryIgnoresNonconformingBatchOverflow() {
  Harness harness;
  harness.listHandler = [](std::size_t) {
    PendingBatchOutcome outcome{
        .storageAvailable = true, .entries = {}, .diagnostic = {}};
    for (std::size_t index = 0; index < 300; ++index) {
      const std::string id = "overflow-" + std::to_string(index);
      outcome.entries.push_back(
          {.status = PendingReadStatus::Found,
           .attemptId = id,
           .value = pending(id, static_cast<int>(index + 1)),
           .diagnostic = {}});
    }
    return outcome;
  };
  Coordinator coordinator(harness.dependencies());

  const RecoverySummary requested = coordinator.recoverAll(3);
  const RecoverySummary capped = coordinator.recoverAll(999);

  assert(requested.attempted == 3);
  assert(requested.saved == 3);
  assert(requested.pending == 0);
  assert(requested.conflicts == 0);
  assert(capped.attempted == 256);
  assert(capped.saved == 256);
  assert(capped.pending == 0);
  assert(capped.conflicts == 0);
  std::vector<std::string> expected{"list:3"};
  for (std::size_t index = 0; index < 3; ++index) {
    expected.push_back("project:overflow-" + std::to_string(index));
    expected.push_back("ack:overflow-" + std::to_string(index) + ":" +
                       std::to_string(index + 1));
  }
  expected.push_back("list:256");
  for (std::size_t index = 0; index < 256; ++index) {
    expected.push_back("project:overflow-" + std::to_string(index));
    expected.push_back("ack:overflow-" + std::to_string(index) + ":" +
                       std::to_string(index + 1));
  }
  assert(harness.events == expected);
  assert(harness.listCalls == 2);
  assert(harness.projectCalls == 259);
  assert(harness.acknowledgeCalls == 259);
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
  testSaveOutcomePresentationSemantics();
  testPersistOrdersStageLoadProjectAcknowledge();
  testStageStorageFailureReturnsTruthfulUnstagedMessage();
  testStageConflictReturnsUnstagedConflictWithoutReceipt();
  testMalformedSuccessfulStageMetadataIsNotDurable();
  testPendingReadFailuresStopBeforeProjection();
  testPendingPayloadMismatchStopsBeforeProjection();
  testProjectionFailureRetainsPendingScore();
  testProjectionConflictReturnsDurablePendingConflict();
  testAcknowledgeFailureIsDurableAndRetryable();
  testAcknowledgeConflictRetainsPendingConflict();
  testRetryAfterAcknowledgementFailureResumesIdempotently();
  testAlreadyAcknowledgedIsIdempotentPersistSuccess();
  testRetrySkipsAlreadyConfirmedReplayAndScore();
  testUnknownPersistStatusesFailClosed();
  testRecoveryContinuesAfterMalformedAndFailedRows();
  testRecoveryMarkFailuresRemainVisibleAndContinue();
  testUnmarkedRecoveryRowIsRetriedOnNextCall();
  testInvalidRecoveryPayloadStopsBeforeProjection();
  testAlreadyAcknowledgedIsIdempotentRecoverySuccess();
  testUnknownRecoveryStatusesFailClosed();
  testMessagesNeverContainInjectedDiagnostics();
  testRecoveryLimitIsExactly256();
  testRecoveryIgnoresNonconformingBatchOverflow();
  testPersistentFirstBatchDoesNotStarveNewValidRow();
  std::cout << "result persistence coordinator tests passed\n";
  return 0;
}
