#include "IrUploadsController.h"

#include <algorithm>
#include <exception>
#include <unordered_map>
#include <utility>

namespace ir_uploads {
namespace {

bool accepted(ir::IrManualBatchItemStatus status) noexcept {
  return status == ir::IrManualBatchItemStatus::Inserted ||
         status == ir::IrManualBatchItemStatus::RetryQueued ||
         status == ir::IrManualBatchItemStatus::AlreadyQueued ||
         status == ir::IrManualBatchItemStatus::AlreadySubmitted;
}

std::string normalizedFailureReason(std::string_view diagnostic,
                                    std::string_view fallback) {
  std::string result = ir::sanitizeDiagnostic(diagnostic);
  return result.empty() ? ir::sanitizeDiagnostic(fallback) : result;
}

} // namespace

std::size_t detail::eraseQueuedReplayIds(
    std::vector<int> &failedReplayIds,
    std::span<const int> queuedReplayIds) {
  std::unordered_set<int> queued;
  queued.reserve(queuedReplayIds.size());
  queued.insert(queuedReplayIds.begin(), queuedReplayIds.end());
  const std::size_t membershipOperations = failedReplayIds.size();
  std::erase_if(failedReplayIds,
                [&](int replayId) { return queued.contains(replayId); });
  return queuedReplayIds.size() + membershipOperations;
}

void DurableEnqueueGate::requestCancellation() noexcept {
  std::lock_guard lock(mutex_);
  if (!enqueueStarted_) {
    cancellationRequested_ = true;
  }
}

std::optional<ir::IrSavedResultBatchUploadResult>
DurableEnqueueGate::executeUnlessCancelled(
    const std::stop_token &stopToken,
    const std::function<ir::IrSavedResultBatchUploadResult()> &execute) {
  std::lock_guard lock(mutex_);
  if (cancellationRequested_ || stopToken.stop_requested() || enqueueStarted_) {
    return std::nullopt;
  }
  enqueueStarted_ = true;
  return execute();
}

PreparationOutcome prepareSelectedCandidates(
    std::span<const ir::IrUploadCandidate> candidates,
    const std::stop_token &stopToken,
    const PreparationDependencies &dependencies,
    std::shared_ptr<DurableEnqueueGate> enqueueGate) noexcept {
  PreparationOutcome outcome;
  outcome.failedReplayIds.reserve(candidates.size());
  outcome.failureReasons.reserve(candidates.size());
  std::unordered_set<int> recordedFailures;
  recordedFailures.reserve(candidates.size());
  for (const auto &candidate : candidates) {
    outcome.failedReplayIds.push_back(candidate.replayId());
  }
  const auto recordFailure = [&](int replayId, std::string_view diagnostic,
                                 std::string_view fallback) {
    if (replayId > 0 && recordedFailures.emplace(replayId).second) {
      outcome.failureReasons.push_back(
          {.replayId = replayId,
           .diagnostic = normalizedFailureReason(diagnostic, fallback)});
    }
  };
  const auto cancel = [&]() {
    outcome.cancelled = true;
    outcome.failureReasons.clear();
  };

  try {
    if (enqueueGate == nullptr) {
      enqueueGate = std::make_shared<DurableEnqueueGate>();
    }
#if !defined(__ANDROID__) || defined(__cpp_lib_jthread)
    std::stop_callback stopCallback(
        stopToken, [enqueueGate] { enqueueGate->requestCancellation(); });
#else
    if (stopToken.stop_requested()) {
      enqueueGate->requestCancellation();
    }
#endif

    if (dependencies.progress) {
      dependencies.progress(0, candidates.size());
    }

    std::vector<ir::IrSubmission> submissions;
    std::vector<int> submissionReplayIds;
    std::vector<std::string> retryAttemptIds;
    std::vector<int> retryReplayIds;
    submissions.reserve(candidates.size());
    submissionReplayIds.reserve(candidates.size());
    retryAttemptIds.reserve(candidates.size());
    retryReplayIds.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      if (stopToken.stop_requested()) {
        cancel();
        return outcome;
      }

      VerificationOutcome verified;
      try {
        if (dependencies.verify) {
          verified = dependencies.verify(candidates[index], stopToken);
        } else {
          verified.diagnostic = "Saved result verification is unavailable.";
        }
      } catch (const std::exception &) {
        verified.diagnostic = "This saved result could not be verified for IR.";
      } catch (...) {
        verified.diagnostic = "This saved result could not be verified for IR.";
      }
      if (stopToken.stop_requested()) {
        cancel();
        return outcome;
      }
      if (verified.submission.has_value() &&
          !verified.retryAttemptId.has_value()) {
        submissionReplayIds.push_back(candidates[index].replayId());
        submissions.push_back(std::move(*verified.submission));
      } else if (!verified.submission.has_value() &&
                 verified.retryAttemptId.has_value() &&
                 candidates[index].replay.attemptId.has_value() &&
                 *verified.retryAttemptId ==
                     *candidates[index].replay.attemptId) {
        retryReplayIds.push_back(candidates[index].replayId());
        retryAttemptIds.push_back(std::move(*verified.retryAttemptId));
      } else {
        recordFailure(candidates[index].replayId(), verified.diagnostic,
                      "This saved result could not be verified for IR.");
      }
      if (dependencies.progress) {
        dependencies.progress(index + 1, candidates.size());
      }
    }

    if (stopToken.stop_requested()) {
      cancel();
      return outcome;
    }
    if (submissions.empty() && retryAttemptIds.empty()) {
      return outcome;
    }
    if (!submissions.empty() && !dependencies.enqueueBatch) {
      for (const int replayId : submissionReplayIds) {
        recordFailure(replayId, {}, "IR batch enqueue is unavailable.");
      }
      submissions.clear();
      submissionReplayIds.clear();
    }
    if (!retryAttemptIds.empty() && !dependencies.retryBatch) {
      for (const int replayId : retryReplayIds) {
        recordFailure(replayId, {}, "IR batch retry is unavailable.");
      }
      retryAttemptIds.clear();
      retryReplayIds.clear();
    }
    if (submissions.empty() && retryAttemptIds.empty()) {
      return outcome;
    }

    std::unordered_map<std::string, std::size_t> submissionCounts;
    submissionCounts.reserve(submissions.size() + retryAttemptIds.size());
    for (const auto &submission : submissions) {
      ++submissionCounts[submission.attemptId];
    }
    for (const auto &attemptId : retryAttemptIds) {
      ++submissionCounts[attemptId];
    }

    std::vector<ir::IrSubmission> uniqueSubmissions;
    std::vector<std::string> uniqueRetryAttemptIds;
    std::vector<std::string> uniqueAttemptIds;
    std::vector<int> uniqueReplayIds;
    uniqueSubmissions.reserve(submissions.size());
    uniqueRetryAttemptIds.reserve(retryAttemptIds.size());
    uniqueAttemptIds.reserve(submissions.size() + retryAttemptIds.size());
    uniqueReplayIds.reserve(submissionReplayIds.size() + retryReplayIds.size());
    for (std::size_t index = 0; index < submissions.size(); ++index) {
      if (submissionCounts[submissions[index].attemptId] == 1) {
        uniqueAttemptIds.push_back(submissions[index].attemptId);
        uniqueReplayIds.push_back(submissionReplayIds[index]);
        uniqueSubmissions.push_back(std::move(submissions[index]));
      } else {
        recordFailure(submissionReplayIds[index], {},
                      "Saved results have duplicate IR attempt identity.");
      }
    }
    for (std::size_t index = 0; index < retryAttemptIds.size(); ++index) {
      if (submissionCounts[retryAttemptIds[index]] == 1) {
        uniqueAttemptIds.push_back(retryAttemptIds[index]);
        uniqueReplayIds.push_back(retryReplayIds[index]);
        uniqueRetryAttemptIds.push_back(std::move(retryAttemptIds[index]));
      } else {
        recordFailure(retryReplayIds[index], {},
                      "Saved results have duplicate IR attempt identity.");
      }
    }
    if (uniqueAttemptIds.empty()) {
      return outcome;
    }

    std::optional<ir::IrSavedResultBatchUploadResult> batch;
    try {
      batch = enqueueGate->executeUnlessCancelled(stopToken, [&] {
        ir::IrSavedResultBatchUploadResult combined;
        const auto append = [&](ir::IrSavedResultBatchUploadResult source) {
          combined.items.insert(combined.items.end(),
                                std::make_move_iterator(source.items.begin()),
                                std::make_move_iterator(source.items.end()));
          combined.buildFailures += source.buildFailures;
          if (combined.diagnostic.empty()) {
            combined.diagnostic = std::move(source.diagnostic);
          }
        };
        if (!uniqueSubmissions.empty()) {
          append(dependencies.enqueueBatch(uniqueSubmissions));
        }
        if (!uniqueRetryAttemptIds.empty()) {
          append(dependencies.retryBatch(uniqueRetryAttemptIds));
        }
        return combined;
      });
    } catch (const std::exception &) {
      for (const int replayId : uniqueReplayIds) {
        recordFailure(replayId, {}, "IR batch enqueue failed.");
      }
      return outcome;
    } catch (...) {
      for (const int replayId : uniqueReplayIds) {
        recordFailure(replayId, {}, "IR batch enqueue failed.");
      }
      return outcome;
    }
    if (!batch.has_value()) {
      cancel();
      return outcome;
    }

    std::unordered_map<std::string, std::size_t> resultCounts;
    std::unordered_map<std::string, const ir::IrManualBatchItemOutcome *>
        resultItems;
    resultCounts.reserve(batch->items.size());
    resultItems.reserve(batch->items.size());
    for (const auto &item : batch->items) {
      ++resultCounts[item.attemptId];
      resultItems.try_emplace(item.attemptId, &item);
    }
    for (std::size_t index = 0; index < uniqueAttemptIds.size(); ++index) {
      const std::string &attemptId = uniqueAttemptIds[index];
      const auto count = resultCounts.find(attemptId);
      const auto item = resultItems.find(attemptId);
      if (count != resultCounts.end() && count->second == 1 &&
          item != resultItems.end() && accepted(item->second->status)) {
        outcome.queuedReplayIds.push_back(uniqueReplayIds[index]);
      } else if (count != resultCounts.end() && count->second > 1) {
        recordFailure(uniqueReplayIds[index], {},
                      "IR batch enqueue returned ambiguous outcomes.");
      } else if (item != resultItems.end()) {
        std::string diagnostic =
            ir::sanitizeDiagnostic(item->second->diagnostic);
        if (diagnostic.empty()) {
          diagnostic = ir::sanitizeDiagnostic(batch->diagnostic);
        }
        recordFailure(uniqueReplayIds[index], diagnostic,
                      "IR batch enqueue rejected this score.");
      } else {
        recordFailure(uniqueReplayIds[index], batch->diagnostic,
                      "IR batch enqueue returned no outcome.");
      }
    }
    (void)detail::eraseQueuedReplayIds(outcome.failedReplayIds,
                                       outcome.queuedReplayIds);
    return outcome;
  } catch (const std::exception &) {
    for (const int replayId : outcome.failedReplayIds) {
      recordFailure(replayId, {}, "IR upload preparation failed.");
    }
    return outcome;
  } catch (...) {
    for (const int replayId : outcome.failedReplayIds) {
      recordFailure(replayId, {}, "IR upload preparation failed.");
    }
    return outcome;
  }
}

void Controller::replaceCandidates(
    std::vector<ir::IrUploadCandidate> candidates) {
  candidates_ = std::move(candidates);
  ir::detail::intersectIrUploadSelectionIndexed(selectedReplayIds_,
                                                candidates_);
  applySessionFailureReasons();
}

void Controller::applySessionFailureReasons() {
  std::unordered_set<int> publishedReplayIds;
  publishedReplayIds.reserve(candidates_.size());
  for (auto &candidate : candidates_) {
    publishedReplayIds.insert(candidate.replayId());
    const auto found = sessionFailureReasons_.find(candidate.replayId());
    if (found != sessionFailureReasons_.end()) {
      candidate.failureReason = found->second;
    }
  }
  std::erase_if(sessionFailureReasons_, [&](const auto &entry) {
    return !publishedReplayIds.contains(entry.first);
  });
}

void Controller::applyCandidateRefresh(
    std::optional<std::vector<ir::IrUploadCandidate>> candidates) {
  if (candidates.has_value()) {
    replaceCandidates(std::move(*candidates));
  }
}

void Controller::toggle(int replayId) {
  if (preparing_) {
    return;
  }
  const auto found = std::ranges::find_if(
      candidates_, [replayId](const ir::IrUploadCandidate &candidate) {
        return candidate.replayId() == replayId;
      });
  if (found == candidates_.end()) {
    return;
  }
  if (!selectedReplayIds_.erase(replayId)) {
    selectedReplayIds_.insert(replayId);
  }
}

void Controller::selectAll() {
  if (preparing_) {
    return;
  }
  selectedReplayIds_.reserve(candidates_.size());
  for (const auto &candidate : candidates_) {
    selectedReplayIds_.insert(candidate.replayId());
  }
}

void Controller::clearSelection() {
  if (!preparing_) {
    selectedReplayIds_.clear();
  }
}

std::vector<ir::IrUploadCandidate> Controller::beginPreparation() {
  if (preparing_ || selectedReplayIds_.empty()) {
    return {};
  }
  std::vector<ir::IrUploadCandidate> snapshot;
  snapshot.reserve(selectedReplayIds_.size());
  for (const auto &candidate : candidates_) {
    if (selectedReplayIds_.contains(candidate.replayId())) {
      snapshot.push_back(candidate);
    }
  }
  if (!snapshot.empty()) {
    preparing_ = true;
    statusText_ = "Preparing 0 of " + std::to_string(snapshot.size()) + "...";
  }
  return snapshot;
}

void Controller::setPreparationProgress(std::size_t completed,
                                        std::size_t total) {
  if (!preparing_) {
    return;
  }
  completed = std::min(completed, total);
  statusText_ = "Preparing " + std::to_string(completed) + " of " +
                std::to_string(total) + "...";
}

void Controller::markCancellationRequested() {
  if (preparing_) {
    statusText_ = "Cancelling...";
  }
}

void Controller::completePreparation(const PreparationOutcome &outcome) {
  if (!preparing_) {
    return;
  }
  selectedReplayIds_.clear();
  selectedReplayIds_.insert(outcome.failedReplayIds.begin(),
                            outcome.failedReplayIds.end());
  preparing_ = false;
  if (outcome.cancelled) {
    statusText_ = "Upload cancelled.";
    return;
  }
  std::unordered_set<int> queuedReplayIds;
  queuedReplayIds.reserve(outcome.queuedReplayIds.size());
  queuedReplayIds.insert(outcome.queuedReplayIds.begin(),
                         outcome.queuedReplayIds.end());
  for (const int replayId : queuedReplayIds) {
    sessionFailureReasons_.erase(replayId);
  }
  std::unordered_set<int> failedReplayIds;
  failedReplayIds.reserve(outcome.failedReplayIds.size());
  failedReplayIds.insert(outcome.failedReplayIds.begin(),
                         outcome.failedReplayIds.end());
  for (const auto &failure : outcome.failureReasons) {
    if (failure.replayId > 0 && failedReplayIds.contains(failure.replayId)) {
      sessionFailureReasons_[failure.replayId] = normalizedFailureReason(
          failure.diagnostic, "IR upload preparation failed.");
    }
  }
  for (auto &candidate : candidates_) {
    if (queuedReplayIds.contains(candidate.replayId())) {
      candidate.failureReason.clear();
      continue;
    }
    const auto found = sessionFailureReasons_.find(candidate.replayId());
    if (found != sessionFailureReasons_.end()) {
      candidate.failureReason = found->second;
    }
  }
  statusText_ = std::to_string(outcome.queuedReplayIds.size()) + " queued, " +
                std::to_string(outcome.failedReplayIds.size()) + " failed";
}

} // namespace ir_uploads
