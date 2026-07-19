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
DurableEnqueueGate::enqueueUnlessCancelled(
    const std::stop_token &stopToken,
    std::span<const ir::IrSubmission> submissions,
    const std::function<ir::IrSavedResultBatchUploadResult(
        std::span<const ir::IrSubmission>)> &enqueueBatch) {
  std::lock_guard lock(mutex_);
  if (cancellationRequested_ || stopToken.stop_requested() || enqueueStarted_) {
    return std::nullopt;
  }
  enqueueStarted_ = true;
  return enqueueBatch(submissions);
}

PreparationOutcome prepareSelectedCandidates(
    std::span<const ir::IrUploadCandidate> candidates,
    const std::stop_token &stopToken,
    const PreparationDependencies &dependencies,
    std::shared_ptr<DurableEnqueueGate> enqueueGate) noexcept {
  PreparationOutcome outcome;
  outcome.failedReplayIds.reserve(candidates.size());
  for (const auto &candidate : candidates) {
    outcome.failedReplayIds.push_back(candidate.replayId());
  }

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
    submissions.reserve(candidates.size());
    submissionReplayIds.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      if (stopToken.stop_requested()) {
        outcome.cancelled = true;
        return outcome;
      }

      VerificationOutcome verified;
      try {
        if (dependencies.verify) {
          verified = dependencies.verify(candidates[index], stopToken);
        }
      } catch (const std::exception &) {
      } catch (...) {
      }
      if (stopToken.stop_requested()) {
        outcome.cancelled = true;
        return outcome;
      }
      if (verified.submission.has_value()) {
        submissionReplayIds.push_back(candidates[index].replayId());
        submissions.push_back(std::move(*verified.submission));
      }
      if (dependencies.progress) {
        dependencies.progress(index + 1, candidates.size());
      }
    }

    if (submissions.empty()) {
      return outcome;
    }
    if (!dependencies.enqueueBatch) {
      return outcome;
    }

    std::unordered_map<std::string, std::size_t> submissionCounts;
    submissionCounts.reserve(submissions.size());
    for (const auto &submission : submissions) {
      ++submissionCounts[submission.attemptId];
    }

    std::vector<ir::IrSubmission> uniqueSubmissions;
    std::vector<int> uniqueSubmissionReplayIds;
    uniqueSubmissions.reserve(submissions.size());
    uniqueSubmissionReplayIds.reserve(submissionReplayIds.size());
    for (std::size_t index = 0; index < submissions.size(); ++index) {
      if (submissionCounts[submissions[index].attemptId] == 1) {
        uniqueSubmissions.push_back(std::move(submissions[index]));
        uniqueSubmissionReplayIds.push_back(submissionReplayIds[index]);
      }
    }
    if (uniqueSubmissions.empty()) {
      return outcome;
    }

    const auto batch = enqueueGate->enqueueUnlessCancelled(
        stopToken, uniqueSubmissions, dependencies.enqueueBatch);
    if (!batch.has_value()) {
      outcome.cancelled = true;
      return outcome;
    }

    std::unordered_map<std::string, std::size_t> resultCounts;
    std::unordered_map<std::string, ir::IrManualBatchItemStatus> resultStatuses;
    resultCounts.reserve(batch->items.size());
    resultStatuses.reserve(batch->items.size());
    for (const auto &item : batch->items) {
      ++resultCounts[item.attemptId];
      resultStatuses.try_emplace(item.attemptId, item.status);
    }
    for (std::size_t index = 0; index < uniqueSubmissions.size(); ++index) {
      const std::string &attemptId = uniqueSubmissions[index].attemptId;
      const auto status = resultStatuses.find(attemptId);
      if (resultCounts[attemptId] == 1 && status != resultStatuses.end() &&
          accepted(status->second)) {
        outcome.queuedReplayIds.push_back(uniqueSubmissionReplayIds[index]);
      }
    }
    (void)detail::eraseQueuedReplayIds(outcome.failedReplayIds,
                                       outcome.queuedReplayIds);
    return outcome;
  } catch (const std::exception &) {
    return outcome;
  } catch (...) {
    return outcome;
  }
}

void Controller::replaceCandidates(
    std::vector<ir::IrUploadCandidate> candidates) {
  candidates_ = std::move(candidates);
  ir::detail::intersectIrUploadSelectionIndexed(selectedReplayIds_,
                                                candidates_);
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
  statusText_ = std::to_string(outcome.queuedReplayIds.size()) + " queued, " +
                std::to_string(outcome.failedReplayIds.size()) + " failed";
}

} // namespace ir_uploads
