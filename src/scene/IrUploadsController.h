#pragma once

#include "../ThreadCompat.h"
#include "../ir/IrSavedResultBatchUpload.h"
#include "../ir/IrUploadCandidates.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ir_uploads {

namespace detail {

[[nodiscard]] std::size_t
eraseQueuedReplayIds(std::vector<int> &failedReplayIds,
                     std::span<const int> queuedReplayIds);

} // namespace detail

struct VerificationOutcome {
  std::optional<ir::IrSubmission> submission;
  std::optional<std::string> retryAttemptId;
  std::string diagnostic;
};

struct PreparationDependencies {
  std::function<VerificationOutcome(const ir::IrUploadCandidate &,
                                    const std::stop_token &)>
      verify;
  std::function<ir::IrSavedResultBatchUploadResult(
      std::span<const ir::IrSubmission>)>
      enqueueBatch;
  std::function<ir::IrSavedResultBatchUploadResult(
      std::span<const std::string>)>
      retryBatch;
  std::function<void(std::size_t completed, std::size_t total)> progress;
};

struct PreparationFailureReason {
  int replayId = 0;
  std::string diagnostic;
};

struct PreparationOutcome {
  bool cancelled = false;
  std::vector<int> queuedReplayIds;
  std::vector<int> failedReplayIds;
  std::vector<PreparationFailureReason> failureReasons;
};

class DurableEnqueueGate {
public:
  void requestCancellation() noexcept;
  [[nodiscard]] std::optional<ir::IrSavedResultBatchUploadResult>
  executeUnlessCancelled(
      const std::stop_token &stopToken,
      const std::function<ir::IrSavedResultBatchUploadResult()> &execute);

private:
  std::mutex mutex_;
  bool cancellationRequested_ = false;
  bool enqueueStarted_ = false;
};

[[nodiscard]] PreparationOutcome prepareSelectedCandidates(
    std::span<const ir::IrUploadCandidate> candidates,
    const std::stop_token &stopToken,
    const PreparationDependencies &dependencies,
    std::shared_ptr<DurableEnqueueGate> enqueueGate = {}) noexcept;

class Controller {
public:
  void replaceCandidates(std::vector<ir::IrUploadCandidate> candidates);
  void applyCandidateRefresh(
      std::optional<std::vector<ir::IrUploadCandidate>> candidates);
  void toggle(int replayId);
  void selectAll();
  void clearSelection();

  [[nodiscard]] std::vector<ir::IrUploadCandidate> beginPreparation();
  void setPreparationProgress(std::size_t completed, std::size_t total);
  void markCancellationRequested();
  void completePreparation(const PreparationOutcome &outcome);

  [[nodiscard]] const std::vector<ir::IrUploadCandidate> &candidates() const {
    return candidates_;
  }
  [[nodiscard]] const std::unordered_set<int> &selectedReplayIds() const {
    return selectedReplayIds_;
  }
  [[nodiscard]] std::size_t selectedCount() const noexcept {
    return selectedReplayIds_.size();
  }
  [[nodiscard]] bool isSelected(int replayId) const noexcept {
    return selectedReplayIds_.contains(replayId);
  }
  [[nodiscard]] bool selectionLocked() const noexcept { return preparing_; }
  [[nodiscard]] const std::string &statusText() const noexcept {
    return statusText_;
  }

private:
  void applySessionFailureReasons();

  std::vector<ir::IrUploadCandidate> candidates_;
  std::unordered_set<int> selectedReplayIds_;
  std::unordered_map<int, std::string> sessionFailureReasons_;
  bool preparing_ = false;
  std::string statusText_;
};

} // namespace ir_uploads
