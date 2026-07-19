#include "IrUploadsController.h"

#include <algorithm>
#include <exception>
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

PreparationOutcome prepareSelectedCandidates(
    std::span<const ir::IrUploadCandidate> candidates,
    const std::stop_token &stopToken,
    const PreparationDependencies &dependencies) noexcept {
  PreparationOutcome outcome;
  outcome.failedReplayIds.reserve(candidates.size());
  for (const auto &candidate : candidates) {
    outcome.failedReplayIds.push_back(candidate.replayId());
  }

  try {
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
      if (dependencies.verify) {
        verified = dependencies.verify(candidates[index], stopToken);
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

    const auto batch = dependencies.enqueueBatch(submissions);
    for (std::size_t index = 0;
         index < submissions.size() && index < batch.items.size(); ++index) {
      if (batch.items[index].attemptId == submissions[index].attemptId &&
          accepted(batch.items[index].status)) {
        outcome.queuedReplayIds.push_back(submissionReplayIds[index]);
      }
    }
    std::erase_if(outcome.failedReplayIds, [&](int replayId) {
      return std::ranges::find(outcome.queuedReplayIds, replayId) !=
             outcome.queuedReplayIds.end();
    });
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
