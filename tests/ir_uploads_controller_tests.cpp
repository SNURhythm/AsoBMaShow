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

std::vector<std::string> attemptIds(std::initializer_list<int> suffixes) {
  std::vector<std::string> result;
  result.reserve(suffixes.size());
  for (const int suffix : suffixes) {
    result.push_back(attemptId(suffix));
  }
  return result;
}

ir::IrUploadCandidate candidate(int resultId) {
  ir::IrUploadCandidate value;
  value.modernChartResultId = resultId;
  value.result.resultId = resultId;
  value.result.attemptId = attemptId(resultId);
  value.result.score.chartTitle = "Candidate " + std::to_string(resultId);
  return value;
}

ir::IrSubmission submissionFor(const ir::IrUploadCandidate &candidate) {
  ir::IrSubmission value;
  value.attemptId = candidate.result.attemptId;
  value.chartMd5 = std::string(32, 'b');
  value.chartSha256 = std::string(64, 'a');
  return value;
}

std::string failureReason(const ir_uploads::PreparationOutcome &outcome,
                          int suffix) {
  const std::string expectedAttemptId = attemptId(suffix);
  const auto found = std::ranges::find_if(
      outcome.failureReasons,
      [&expectedAttemptId](
          const ir_uploads::PreparationFailureReason &failure) {
        return failure.attemptId == expectedAttemptId;
      });
  return found == outcome.failureReasons.end() ? std::string{}
                                               : found->diagnostic;
}

void testSelectionSnapshotLockAndFinalSummary() {
  ir_uploads::Controller controller;
  controller.replaceCandidates({candidate(1), candidate(2), candidate(3)});
  controller.selectAll();
  controller.toggle(attemptId(2));
  expect(controller.selectedCount() == 2,
         "row toggles mutate replay-ID selection");

  const auto snapshot = controller.beginPreparation();
  expect(
      snapshot.size() == 2 && snapshot[0].attemptId() == attemptId(1) &&
          snapshot[1].attemptId() == attemptId(3) &&
          controller.selectionLocked(),
      "preparation snapshots selected rows in display order and locks input");
  controller.clearSelection();
  expect(controller.selectedCount() == 2,
         "locked selection ignores toolbar mutations");

  controller.setPreparationProgress(1, 2);
  expect(controller.statusText() == "Preparing 1 of 2...",
         "preparation progress has a bounded live label");

  ir_uploads::PreparationOutcome outcome;
  outcome.queuedAttemptIds = attemptIds({1});
  outcome.failedAttemptIds = attemptIds({3});
  controller.completePreparation(outcome);
  expect(!controller.selectionLocked() && controller.selectedCount() == 1 &&
             controller.isSelected(attemptId(3)) &&
             !controller.isSelected(attemptId(1)),
         "completion removes queued rows and retains failed selections");
  expect(controller.statusText() == "1 queued, 1 failed",
         "completion publishes the exact queued/failed summary");

  controller.replaceCandidates({candidate(3), candidate(4)});
  expect(controller.isSelected(attemptId(3)) &&
             !controller.isSelected(attemptId(4)),
         "refresh intersects selection with the published candidate set");
}

void testFailedRefreshPreservesPublishedCandidatesAndSelection() {
  ir_uploads::Controller controller;
  controller.replaceCandidates({candidate(4), candidate(5)});
  controller.toggle(attemptId(5));

  controller.applyCandidateRefresh(std::nullopt);

  expect(controller.candidates().size() == 2 &&
             controller.candidates()[0].attemptId() == attemptId(4) &&
             controller.candidates()[1].attemptId() == attemptId(5) &&
             controller.selectedCount() == 1 &&
             controller.isSelected(attemptId(5)),
         "a failed repository refresh preserves the last published rows and "
         "selection");
}

void testPreparationContinuesAfterFailureAndBatchesOnce() {
  const std::vector candidates{candidate(10), candidate(11), candidate(12)};
  std::vector<std::string> verified;
  std::vector<std::pair<std::size_t, std::size_t>> progress;
  int batchCalls = 0;

  ir_uploads::PreparationDependencies dependencies;
  dependencies.verify = [&](const ir::IrUploadCandidate &value,
                            const std::stop_token &) {
    verified.emplace_back(value.attemptId());
    if (value.attemptId() == attemptId(11)) {
      return ir_uploads::VerificationOutcome{
          .diagnostic = "This saved result could not be verified for IR."};
    }
    return ir_uploads::VerificationOutcome{.submission = submissionFor(value)};
  };
  dependencies.enqueueBatch =
      [&](std::span<const ir::IrSubmission> submissions) {
        ++batchCalls;
        expect(verified == attemptIds({10, 11, 12}),
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
  expect(outcome.queuedAttemptIds == attemptIds({10, 12}) &&
             outcome.failedAttemptIds == attemptIds({11}),
         "preparation isolates verification failure and maps batch results");
  expect(outcome.failureReasons.size() == 1 &&
             outcome.failureReasons.front().attemptId == attemptId(11) &&
             outcome.failureReasons.front().diagnostic ==
                 "This saved result could not be verified for IR.",
         "preparation preserves the failed verifier reason by attempt ID");
  expect(progress == std::vector<std::pair<std::size_t, std::size_t>>(
                         {{0, 3}, {1, 3}, {2, 3}, {3, 3}}),
         "preparation reports deterministic per-result progress");
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
  expect(outcome.failedAttemptIds == attemptIds({20, 21, 22}),
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
             outcome.failedAttemptIds == attemptIds({23, 24}),
         "cancellation in the final pre-enqueue window makes zero batch calls");
}

void testThrowingVerifierDoesNotAbortOtherCandidates() {
  const std::vector candidates{candidate(30), candidate(31), candidate(32)};
  std::vector<std::string> verified;
  int batchCalls = 0;

  ir_uploads::PreparationDependencies dependencies;
  dependencies.verify = [&](const ir::IrUploadCandidate &value,
                            const std::stop_token &) {
    verified.emplace_back(value.attemptId());
    if (value.attemptId() == attemptId(31)) {
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
  expect(verified == attemptIds({30, 31, 32}) && batchCalls == 1,
         "a throwing verifier is isolated to its candidate");
  expect(outcome.queuedAttemptIds == attemptIds({30, 32}) &&
             outcome.failedAttemptIds == attemptIds({31}),
         "throw isolation preserves earlier and later verified submissions");
  expect(outcome.failureReasons.size() == 1 &&
             outcome.failureReasons.front().attemptId == attemptId(31) &&
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
  expect(outcome.queuedAttemptIds == attemptIds({40, 42}) &&
             outcome.failedAttemptIds == attemptIds({41}),
         "reordered batch outcomes map to candidates by attempt ID");
  expect(outcome.failureReasons.size() == 1 &&
             outcome.failureReasons.front().attemptId == attemptId(41) &&
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
  expect(outcome.queuedAttemptIds == attemptIds({52}) &&
             outcome.failedAttemptIds == attemptIds({50, 51}),
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
  expect(controller.selectedCount() == 2 &&
             controller.isSelected(attemptId(50)) &&
             controller.isSelected(attemptId(51)) &&
             !controller.isSelected(attemptId(52)),
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
  expect(outcome.queuedAttemptIds == attemptIds({61}) &&
             outcome.failedAttemptIds == attemptIds({60}),
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
  controller.toggle(attemptId(70));
  (void)controller.beginPreparation();
  controller.completePreparation({
      .failedAttemptIds = attemptIds({70}),
      .failureReasons = {{.attemptId = attemptId(70),
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
  controller.completePreparation({.queuedAttemptIds = attemptIds({70})});
  controller.replaceCandidates({failed});
  expect(controller.candidates().front().failureReason ==
             "older server failure",
         "successful queueing clears only the session override");
}

void testCancellationPreservesExistingSessionFailureReason() {
  ir_uploads::Controller controller;
  auto failed = candidate(71);
  controller.replaceCandidates({failed});
  controller.toggle(attemptId(71));
  (void)controller.beginPreparation();
  controller.completePreparation({
      .failedAttemptIds = attemptIds({71}),
      .failureReasons = {{.attemptId = attemptId(71),
                          .diagnostic = "existing verification failure"}},
  });

  (void)controller.beginPreparation();
  controller.completePreparation({
      .cancelled = true,
      .failedAttemptIds = attemptIds({71}),
      .failureReasons = {{.attemptId = attemptId(71),
                          .diagnostic = "cancelled replacement"}},
  });
  controller.replaceCandidates({failed});

  expect(controller.candidates().front().failureReason ==
             "existing verification failure",
         "cancellation keeps the previously visible session reason");
}

void testMaximumQueuedAttemptFilterHasLinearOperationCount() {
  constexpr std::size_t count = ir::kMaximumIrUploadCandidateRows;
  std::vector<std::string> failedAttemptIds;
  std::vector<std::string> queuedAttemptIds;
  failedAttemptIds.reserve(count);
  queuedAttemptIds.reserve(count / 2);
  for (std::size_t index = 0; index < count; ++index) {
    failedAttemptIds.push_back("attempt-" + std::to_string(index + 1));
  }
  for (std::size_t suffix = count; suffix > 0; suffix -= 2) {
    queuedAttemptIds.push_back("attempt-" + std::to_string(suffix));
  }

  const std::size_t operations = ir_uploads::detail::eraseQueuedAttemptIds(
      failedAttemptIds, queuedAttemptIds);

  bool exact = failedAttemptIds.size() == count / 2;
  for (std::size_t index = 0; exact && index < failedAttemptIds.size();
       ++index) {
    exact = failedAttemptIds[index] ==
            "attempt-" + std::to_string(index * 2 + 1);
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
  testCancellationStopsBeforeBatchAndRetainsUntouchedRows();
  testCancellationAfterFinalVerificationStopsBeforeBatch();
  testThrowingVerifierDoesNotAbortOtherCandidates();
  testBatchOutcomesMapByAttemptId();
  testCompactedBatchOutcomesFailClosed();
  testDuplicateBatchOutcomesFailClosed();
  testSessionFailureReasonsOverrideRefreshAndClearAfterQueue();
  testCancellationPreservesExistingSessionFailureReason();
  testMaximumQueuedAttemptFilterHasLinearOperationCount();
  if (failures != 0) {
    std::cerr << failures << " IR uploads controller test(s) failed\n";
    return 1;
  }
  return 0;
}
