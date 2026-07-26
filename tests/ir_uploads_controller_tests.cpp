#include "scene/IrUploadsController.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <stop_token>
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

std::string attemptId(int suffix) {
  char value[37]{};
  std::snprintf(value, sizeof(value), "123e4567-e89b-42d3-a456-426614174%03d",
                suffix);
  return value;
}

ir::IrUploadCandidate candidate(int replayId) {
  ir::IrUploadCandidate value;
  value.replay.id = replayId;
  value.replay.attemptId = attemptId(replayId);
  value.chart.meta.Title = "Candidate " + std::to_string(replayId);
  return value;
}

ir::IrSubmission submissionFor(const ir::IrUploadCandidate &candidate) {
  ir::IrSubmission value;
  value.attemptId = *candidate.replay.attemptId;
  value.chartMd5 = std::string(32, 'b');
  value.chartSha256 = std::string(64, 'a');
  return value;
}

std::string failureReason(const ir_uploads::PreparationOutcome &outcome,
                          int replayId) {
  const auto found = std::ranges::find_if(
      outcome.failureReasons,
      [replayId](const ir_uploads::PreparationFailureReason &failure) {
        return failure.replayId == replayId;
      });
  return found == outcome.failureReasons.end() ? std::string{}
                                               : found->diagnostic;
}

void testSelectionSnapshotLockAndFinalSummary() {
  ir_uploads::Controller controller;
  controller.replaceCandidates({candidate(1), candidate(2), candidate(3)});
  controller.selectAll();
  controller.toggle(2);
  expect(controller.selectedCount() == 2,
         "row toggles mutate replay-ID selection");

  const auto snapshot = controller.beginPreparation();
  expect(
      snapshot.size() == 2 && snapshot[0].replayId() == 1 &&
          snapshot[1].replayId() == 3 && controller.selectionLocked(),
      "preparation snapshots selected rows in display order and locks input");
  controller.clearSelection();
  expect(controller.selectedCount() == 2,
         "locked selection ignores toolbar mutations");

  controller.setPreparationProgress(1, 2);
  expect(controller.statusText() == "Preparing 1 of 2...",
         "preparation progress has a bounded live label");

  ir_uploads::PreparationOutcome outcome;
  outcome.queuedReplayIds = {1};
  outcome.failedReplayIds = {3};
  controller.completePreparation(outcome);
  expect(!controller.selectionLocked() && controller.selectedCount() == 1 &&
             controller.isSelected(3) && !controller.isSelected(1),
         "completion removes queued rows and retains failed selections");
  expect(controller.statusText() == "1 queued, 1 failed",
         "completion publishes the exact queued/failed summary");

  controller.replaceCandidates({candidate(3), candidate(4)});
  expect(controller.isSelected(3) && !controller.isSelected(4),
         "refresh intersects selection with the published candidate set");
}

void testFailedRefreshPreservesPublishedCandidatesAndSelection() {
  ir_uploads::Controller controller;
  controller.replaceCandidates({candidate(4), candidate(5)});
  controller.toggle(5);

  controller.applyCandidateRefresh(std::nullopt);

  expect(controller.candidates().size() == 2 &&
             controller.candidates()[0].replayId() == 4 &&
             controller.candidates()[1].replayId() == 5 &&
             controller.selectedCount() == 1 && controller.isSelected(5),
         "a failed repository refresh preserves the last published rows and "
         "selection");
}

void testPreparationContinuesAfterFailureAndBatchesOnce() {
  const std::vector candidates{candidate(10), candidate(11), candidate(12)};
  std::vector<int> verified;
  std::vector<std::pair<std::size_t, std::size_t>> progress;
  int batchCalls = 0;

  ir_uploads::PreparationDependencies dependencies;
  dependencies.verify = [&](const ir::IrUploadCandidate &value,
                            const std::stop_token &) {
    verified.push_back(value.replayId());
    if (value.replayId() == 11) {
      return ir_uploads::VerificationOutcome{
          .diagnostic = "This saved result could not be verified for IR."};
    }
    return ir_uploads::VerificationOutcome{.submission = submissionFor(value)};
  };
  dependencies.enqueueBatch =
      [&](std::span<const ir::IrSubmission> submissions) {
        ++batchCalls;
        expect(verified == std::vector<int>({10, 11, 12}),
               "every local reconstruction finishes before durable enqueue");
        expect(submissions.size() == 2,
               "only verified submissions cross the batch boundary");
        ir::IrSavedResultBatchUploadResult result;
        result.items = {
            {.attemptId = submissions[0].attemptId,
             .status = ir::IrManualBatchItemStatus::Inserted},
            {.attemptId = submissions[1].attemptId,
             .status = ir::IrManualBatchItemStatus::AlreadyQueued},
        };
        return result;
      };
  dependencies.progress = [&](std::size_t completed, std::size_t total) {
    progress.emplace_back(completed, total);
  };

  std::stop_source stop;
  const auto outcome = ir_uploads::prepareSelectedCandidates(
      candidates, stop.get_token(), dependencies);
  expect(batchCalls == 1,
         "preparation invokes one durable batch enqueue for the selection");
  expect(outcome.queuedReplayIds == std::vector<int>({10, 12}) &&
             outcome.failedReplayIds == std::vector<int>({11}),
         "preparation isolates verification failure and maps batch results");
  expect(outcome.failureReasons.size() == 1 &&
             outcome.failureReasons.front().replayId == 11 &&
             outcome.failureReasons.front().diagnostic ==
                 "This saved result could not be verified for IR.",
         "preparation preserves the failed verifier reason by replay ID");
  expect(progress == std::vector<std::pair<std::size_t, std::size_t>>(
                         {{0, 3}, {1, 3}, {2, 3}, {3, 3}}),
         "preparation reports deterministic per-result progress");
}

void testExistingOutboxRetriesWithoutSnapshotSubmission() {
  auto failed = candidate(13);
  failed.state = ir::IrRecordState::Failed;
  failed.replay.hasIrSubmissionSnapshot = false;
  failed.replay.requestedIrOutboxState = ir::IrOutboxState::FailedPermanent;
  const std::vector candidates{failed};
  int retryBatchCalls = 0;
  int freshBatchCalls = 0;

  ir_uploads::PreparationDependencies dependencies;
  dependencies.verify = [](const ir::IrUploadCandidate &value,
                           const std::stop_token &) {
    return ir_uploads::VerificationOutcome{.retryAttemptId =
                                               *value.replay.attemptId};
  };
  dependencies.retryBatch = [&](std::span<const std::string> attemptIds) {
    ++retryBatchCalls;
    expect(attemptIds.size() == 1 && attemptIds.front() == attemptId(13),
           "bulk retry uses only the persisted outbox attempt identity");
    ir::IrSavedResultBatchUploadResult result;
    result.items = {
        {.attemptId = attemptId(13),
         .status = ir::IrManualBatchItemStatus::RetryQueued},
    };
    return result;
  };
  dependencies.enqueueBatch = [&](std::span<const ir::IrSubmission>) {
    ++freshBatchCalls;
    return ir::IrSavedResultBatchUploadResult{};
  };

  std::stop_source stop;
  const auto outcome = ir_uploads::prepareSelectedCandidates(
      candidates, stop.get_token(), dependencies);
  expect(retryBatchCalls == 1 && freshBatchCalls == 0,
         "a persisted outbox retry bypasses fresh draft enqueue");
  expect(outcome.queuedReplayIds == std::vector<int>{13} &&
             outcome.failedReplayIds.empty() && outcome.failureReasons.empty(),
         "a no-snapshot persisted outbox retry completes as queued");
}

void testCancellationStopsBeforeBatchAndRetainsUntouchedRows() {
  const std::vector candidates{candidate(20), candidate(21), candidate(22)};
  std::stop_source stop;
  int verifyCalls = 0;
  int batchCalls = 0;

  ir_uploads::PreparationDependencies dependencies;
  dependencies.verify = [&](const ir::IrUploadCandidate &value,
                            const std::stop_token &) {
    ++verifyCalls;
    stop.request_stop();
    return ir_uploads::VerificationOutcome{.submission = submissionFor(value)};
  };
  dependencies.enqueueBatch = [&](std::span<const ir::IrSubmission>) {
    ++batchCalls;
    return ir::IrSavedResultBatchUploadResult{};
  };

  const auto outcome = ir_uploads::prepareSelectedCandidates(
      candidates, stop.get_token(), dependencies);
  expect(
      outcome.cancelled && verifyCalls == 1 && batchCalls == 0,
      "cancellation stops after the current verification and before enqueue");
  expect(outcome.failedReplayIds == std::vector<int>({20, 21, 22}),
         "cancelled preparation keeps every unqueued row selected");

  ir_uploads::Controller controller;
  controller.replaceCandidates(candidates);
  controller.selectAll();
  (void)controller.beginPreparation();
  controller.markCancellationRequested();
  expect(controller.statusText() == "Cancelling...",
         "Back publishes cancellation progress while joining");
  controller.completePreparation(outcome);
  expect(controller.statusText() == "Upload cancelled." &&
             controller.selectedCount() == 3,
         "cancelled completion unlocks controls and retains the selection");
}

void testCancellationAfterFinalVerificationStopsBeforeBatch() {
  const std::vector candidates{candidate(23), candidate(24)};
  std::stop_source stop;
  int batchCalls = 0;

  ir_uploads::PreparationDependencies dependencies;
  dependencies.verify = [](const ir::IrUploadCandidate &value,
                           const std::stop_token &) {
    return ir_uploads::VerificationOutcome{.submission = submissionFor(value)};
  };
  dependencies.progress = [&](std::size_t completed, std::size_t total) {
    if (completed == total) {
      stop.request_stop();
    }
  };
  dependencies.enqueueBatch = [&](std::span<const ir::IrSubmission>) {
    ++batchCalls;
    return ir::IrSavedResultBatchUploadResult{};
  };

  const auto outcome = ir_uploads::prepareSelectedCandidates(
      candidates, stop.get_token(), dependencies);
  expect(outcome.cancelled && batchCalls == 0 &&
             outcome.failedReplayIds == std::vector<int>({23, 24}),
         "cancellation in the final pre-enqueue window makes zero batch calls");
}

void testThrowingVerifierDoesNotAbortOtherCandidates() {
  const std::vector candidates{candidate(30), candidate(31), candidate(32)};
  std::vector<int> verified;
  int batchCalls = 0;

  ir_uploads::PreparationDependencies dependencies;
  dependencies.verify = [&](const ir::IrUploadCandidate &value,
                            const std::stop_token &) {
    verified.push_back(value.replayId());
    if (value.replayId() == 31) {
      throw std::runtime_error("broken replay");
    }
    return ir_uploads::VerificationOutcome{.submission = submissionFor(value)};
  };
  dependencies.enqueueBatch =
      [&](std::span<const ir::IrSubmission> submissions) {
        ++batchCalls;
        expect(submissions.size() == 2 &&
                   submissions[0].attemptId == attemptId(30) &&
                   submissions[1].attemptId == attemptId(32),
               "one batch contains valid submissions before and after a throw");
        ir::IrSavedResultBatchUploadResult result;
        result.items = {
            {.attemptId = attemptId(30),
             .status = ir::IrManualBatchItemStatus::Inserted},
            {.attemptId = attemptId(32),
             .status = ir::IrManualBatchItemStatus::Inserted},
        };
        return result;
      };

  std::stop_source stop;
  const auto outcome = ir_uploads::prepareSelectedCandidates(
      candidates, stop.get_token(), dependencies);
  expect(verified == std::vector<int>({30, 31, 32}) && batchCalls == 1,
         "a throwing verifier is isolated to its candidate");
  expect(outcome.queuedReplayIds == std::vector<int>({30, 32}) &&
             outcome.failedReplayIds == std::vector<int>({31}),
         "throw isolation preserves earlier and later verified submissions");
  expect(outcome.failureReasons.size() == 1 &&
             outcome.failureReasons.front().replayId == 31 &&
             outcome.failureReasons.front().diagnostic ==
                 "This saved result could not be verified for IR.",
         "throw isolation publishes a safe verification fallback");
}

void testBatchOutcomesMapByAttemptId() {
  const std::vector candidates{candidate(40), candidate(41), candidate(42)};
  ir_uploads::PreparationDependencies dependencies;
  dependencies.verify = [](const ir::IrUploadCandidate &value,
                           const std::stop_token &) {
    return ir_uploads::VerificationOutcome{.submission = submissionFor(value)};
  };
  dependencies.enqueueBatch = [](std::span<const ir::IrSubmission>) {
    ir::IrSavedResultBatchUploadResult result;
    result.items = {
        {.attemptId = attemptId(42),
         .status = ir::IrManualBatchItemStatus::AlreadyQueued},
        {.attemptId = attemptId(40),
         .status = ir::IrManualBatchItemStatus::Inserted},
        {.attemptId = attemptId(41),
         .status = ir::IrManualBatchItemStatus::Failed,
         .diagnostic = "provider rejected this score"},
    };
    return result;
  };

  std::stop_source stop;
  const auto outcome = ir_uploads::prepareSelectedCandidates(
      candidates, stop.get_token(), dependencies);
  expect(outcome.queuedReplayIds == std::vector<int>({40, 42}) &&
             outcome.failedReplayIds == std::vector<int>({41}),
         "reordered batch outcomes map to candidates by attempt ID");
  expect(outcome.failureReasons.size() == 1 &&
             outcome.failureReasons.front().replayId == 41 &&
             outcome.failureReasons.front().diagnostic ==
                 "provider rejected this score",
         "reordered batch diagnostics map through attempt identity");
}

void testCompactedBatchOutcomesFailClosed() {
  const std::vector candidates{candidate(50), candidate(51), candidate(52)};
  ir_uploads::PreparationDependencies dependencies;
  dependencies.verify = [](const ir::IrUploadCandidate &value,
                           const std::stop_token &) {
    return ir_uploads::VerificationOutcome{.submission = submissionFor(value)};
  };
  dependencies.enqueueBatch = [](std::span<const ir::IrSubmission>) {
    ir::IrSavedResultBatchUploadResult result;
    result.items = {
        {.attemptId = attemptId(52),
         .status = ir::IrManualBatchItemStatus::Inserted},
    };
    return result;
  };

  std::stop_source stop;
  const auto outcome = ir_uploads::prepareSelectedCandidates(
      candidates, stop.get_token(), dependencies);
  expect(outcome.queuedReplayIds == std::vector<int>({52}) &&
             outcome.failedReplayIds == std::vector<int>({50, 51}),
         "missing compacted outcomes remain failed while returned IDs map "
         "accurately");
  expect(
      failureReason(outcome, 50) == "IR batch enqueue returned no outcome." &&
          failureReason(outcome, 51) == "IR batch enqueue returned no outcome.",
      "missing compacted outcomes receive a fail-closed reason");

  ir_uploads::Controller controller;
  controller.replaceCandidates(candidates);
  controller.selectAll();
  (void)controller.beginPreparation();
  controller.completePreparation(outcome);
  expect(controller.selectedCount() == 2 && controller.isSelected(50) &&
             controller.isSelected(51) && !controller.isSelected(52),
         "compacted outcomes retain only unresolved rows in selection");
}

void testDuplicateBatchOutcomesFailClosed() {
  const std::vector candidates{candidate(60), candidate(61)};
  ir_uploads::PreparationDependencies dependencies;
  dependencies.verify = [](const ir::IrUploadCandidate &value,
                           const std::stop_token &) {
    return ir_uploads::VerificationOutcome{.submission = submissionFor(value)};
  };
  dependencies.enqueueBatch = [](std::span<const ir::IrSubmission>) {
    ir::IrSavedResultBatchUploadResult result;
    result.items = {
        {.attemptId = attemptId(60),
         .status = ir::IrManualBatchItemStatus::Inserted},
        {.attemptId = attemptId(60),
         .status = ir::IrManualBatchItemStatus::AlreadyQueued},
        {.attemptId = attemptId(61),
         .status = ir::IrManualBatchItemStatus::Inserted},
    };
    return result;
  };

  std::stop_source stop;
  const auto outcome = ir_uploads::prepareSelectedCandidates(
      candidates, stop.get_token(), dependencies);
  expect(outcome.queuedReplayIds == std::vector<int>({61}) &&
             outcome.failedReplayIds == std::vector<int>({60}),
         "duplicate outcomes fail closed for the ambiguous attempt ID");
  expect(failureReason(outcome, 60) ==
             "IR batch enqueue returned ambiguous outcomes.",
         "duplicate outcomes receive an ambiguity reason");
}

void testSessionFailureReasonsOverrideRefreshAndClearAfterQueue() {
  ir_uploads::Controller controller;
  auto failed = candidate(70);
  failed.failureReason = "older server failure";
  controller.replaceCandidates({failed});
  controller.toggle(70);
  (void)controller.beginPreparation();
  controller.completePreparation({
      .failedReplayIds = {70},
      .failureReasons = {{.replayId = 70,
                          .diagnostic = "new verification failure"}},
  });
  expect(controller.candidates().front().failureReason ==
             "new verification failure",
         "latest session reason overrides durable state");

  controller.replaceCandidates({failed});
  expect(controller.candidates().front().failureReason ==
             "new verification failure",
         "candidate refresh preserves the page-session reason");

  (void)controller.beginPreparation();
  controller.completePreparation({.queuedReplayIds = {70}});
  controller.replaceCandidates({failed});
  expect(controller.candidates().front().failureReason ==
             "older server failure",
         "successful queueing clears only the session override");
}

void testCancellationPreservesExistingSessionFailureReason() {
  ir_uploads::Controller controller;
  auto failed = candidate(71);
  controller.replaceCandidates({failed});
  controller.toggle(71);
  (void)controller.beginPreparation();
  controller.completePreparation({
      .failedReplayIds = {71},
      .failureReasons = {{.replayId = 71,
                          .diagnostic = "existing verification failure"}},
  });

  (void)controller.beginPreparation();
  controller.completePreparation({
      .cancelled = true,
      .failedReplayIds = {71},
      .failureReasons = {{.replayId = 71,
                          .diagnostic = "cancelled replacement"}},
  });
  controller.replaceCandidates({failed});

  expect(controller.candidates().front().failureReason ==
             "existing verification failure",
         "cancellation keeps the previously visible session reason");
}

void testMaximumQueuedReplayFilterHasLinearOperationCount() {
  constexpr std::size_t count = kMaximumIrUploadCandidateRows;
  std::vector<int> failedReplayIds;
  std::vector<int> queuedReplayIds;
  failedReplayIds.reserve(count);
  queuedReplayIds.reserve(count / 2);
  for (std::size_t index = 0; index < count; ++index) {
    failedReplayIds.push_back(static_cast<int>(index + 1));
  }
  for (std::size_t replayId = count; replayId > 0; replayId -= 2) {
    queuedReplayIds.push_back(static_cast<int>(replayId));
  }

  const std::size_t operations = ir_uploads::detail::eraseQueuedReplayIds(
      failedReplayIds, queuedReplayIds);

  bool exact = failedReplayIds.size() == count / 2;
  for (std::size_t index = 0; exact && index < failedReplayIds.size(); ++index) {
    exact = failedReplayIds[index] == static_cast<int>(index * 2 + 1);
  }
  expect(exact, "maximum-bound queued filtering preserves failed input order");
  expect(operations == count + count / 2,
         "maximum-bound queued filtering performs one index and one membership "
         "operation per supplied row");
}

} // namespace

int main() {
  testSelectionSnapshotLockAndFinalSummary();
  testFailedRefreshPreservesPublishedCandidatesAndSelection();
  testPreparationContinuesAfterFailureAndBatchesOnce();
  testExistingOutboxRetriesWithoutSnapshotSubmission();
  testCancellationStopsBeforeBatchAndRetainsUntouchedRows();
  testCancellationAfterFinalVerificationStopsBeforeBatch();
  testThrowingVerifierDoesNotAbortOtherCandidates();
  testBatchOutcomesMapByAttemptId();
  testCompactedBatchOutcomesFailClosed();
  testDuplicateBatchOutcomesFailClosed();
  testSessionFailureReasonsOverrideRefreshAndClearAfterQueue();
  testCancellationPreservesExistingSessionFailureReason();
  testMaximumQueuedReplayFilterHasLinearOperationCount();
  if (failures != 0) {
    std::cerr << failures << " IR uploads controller test(s) failed\n";
    return 1;
  }
  return 0;
}
