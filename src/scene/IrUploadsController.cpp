#include "../i18n/Localization.h"
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

ProviderAvailability
evaluateProviderAvailability(const ProviderAvailabilityInput &input) {
  if (!input.enabled) {
    return {.statusText =
                i18n::message("ir.upload.bokutachi_disabled_enable_before_uploading.message")};
  }
  if (!input.hasCredential) {
    return {.statusText =
                i18n::message("ir.upload.bokutachi_api_key_required_before_uploading.message")};
  }
  if (!input.httpsOrigin) {
    return {.statusText =
                i18n::message("ir.upload.use_https_server_origin_before_uploading.message")};
  }
  if (!input.driverCanSubmit || !input.submissionServiceAvailable) {
    return {.statusText = i18n::message("ir.upload.bokutachi_score_submission_unavailable.message")};
  }
  return {
      .canSubmit = true,
      .statusText = i18n::message("ir.upload.ready_queue_verified_scores_batch_delivery.message"),
  };
}

std::size_t detail::eraseQueuedAttemptIds(
    std::vector<std::string> &failedAttemptIds,
    std::span<const std::string> queuedAttemptIds) {
  std::unordered_set<std::string> queued;
  queued.reserve(queuedAttemptIds.size());
  queued.insert(queuedAttemptIds.begin(), queuedAttemptIds.end());
  const std::size_t membershipOperations = failedAttemptIds.size();
  std::erase_if(failedAttemptIds, [&](const std::string &attemptId) {
    return queued.contains(attemptId);
  });
  return queuedAttemptIds.size() + membershipOperations;
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
  outcome.failedAttemptIds.reserve(candidates.size());
  outcome.failureReasons.reserve(candidates.size());
  std::unordered_set<std::string> recordedFailures;
  recordedFailures.reserve(candidates.size());
  for (const auto &candidate : candidates) {
    outcome.failedAttemptIds.emplace_back(candidate.attemptId());
  }
  const auto recordFailure = [&](std::string_view attemptId,
                                 std::string_view diagnostic,
                                 std::string_view fallback) {
    if (!attemptId.empty() &&
        recordedFailures.emplace(attemptId).second) {
      outcome.failureReasons.push_back(
          {.attemptId = std::string(attemptId),
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
    std::vector<std::string> submissionAttemptIds;
    submissions.reserve(candidates.size());
    submissionAttemptIds.reserve(candidates.size());
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
          verified.diagnostic = i18n::tr("ir.upload.saved_result_verification_unavailable.message");
        }
      } catch (const std::exception &) {
        verified.diagnostic = i18n::tr("ir.upload.saved_result_failed_verified_ir.message");
      } catch (...) {
        verified.diagnostic = i18n::tr("ir.upload.saved_result_failed_verified_ir.message");
      }
      if (stopToken.stop_requested()) {
        cancel();
        return outcome;
      }
      if (verified.submission.has_value()) {
        submissionAttemptIds.emplace_back(candidates[index].attemptId());
        submissions.push_back(std::move(*verified.submission));
      } else {
        recordFailure(candidates[index].attemptId(), verified.diagnostic,
                      i18n::tr("ir.upload.saved_result_failed_verified_ir.message"));
      }
      if (dependencies.progress) {
        dependencies.progress(index + 1, candidates.size());
      }
    }

    if (stopToken.stop_requested()) {
      cancel();
      return outcome;
    }
    if (submissions.empty()) {
      return outcome;
    }
    if (!dependencies.enqueueBatch) {
      for (const std::string &attemptId : submissionAttemptIds) {
        recordFailure(attemptId, {}, i18n::tr("ir.upload.ir_batch_enqueue_unavailable.message"));
      }
      return outcome;
    }

    std::unordered_map<std::string, std::size_t> submissionCounts;
    submissionCounts.reserve(submissions.size());
    for (const auto &submission : submissions) {
      ++submissionCounts[submission.attemptId];
    }

    std::vector<ir::IrSubmission> uniqueSubmissions;
    std::vector<std::string> uniqueSubmissionAttemptIds;
    uniqueSubmissions.reserve(submissions.size());
    uniqueSubmissionAttemptIds.reserve(submissionAttemptIds.size());
    for (std::size_t index = 0; index < submissions.size(); ++index) {
      if (submissionCounts[submissions[index].attemptId] == 1) {
        uniqueSubmissions.push_back(std::move(submissions[index]));
        uniqueSubmissionAttemptIds.push_back(submissionAttemptIds[index]);
      } else {
        recordFailure(submissionAttemptIds[index], {},
                      i18n::tr("ir.upload.duplicate_attempt.message"));
      }
    }
    if (uniqueSubmissions.empty()) {
      return outcome;
    }

    std::optional<ir::IrSavedResultBatchUploadResult> batch;
    try {
      batch = enqueueGate->enqueueUnlessCancelled(stopToken, uniqueSubmissions,
                                                  dependencies.enqueueBatch);
    } catch (const std::exception &) {
      for (const std::string &attemptId : uniqueSubmissionAttemptIds) {
        recordFailure(attemptId, {}, i18n::tr("ir.upload.enqueue_failed.message"));
      }
      return outcome;
    } catch (...) {
      for (const std::string &attemptId : uniqueSubmissionAttemptIds) {
        recordFailure(attemptId, {}, i18n::tr("ir.upload.enqueue_failed.message"));
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
    for (std::size_t index = 0; index < uniqueSubmissions.size(); ++index) {
      const std::string &attemptId = uniqueSubmissions[index].attemptId;
      const auto count = resultCounts.find(attemptId);
      const auto item = resultItems.find(attemptId);
      if (count != resultCounts.end() && count->second == 1 &&
          item != resultItems.end() && accepted(item->second->status)) {
        outcome.queuedAttemptIds.push_back(
            uniqueSubmissionAttemptIds[index]);
      } else if (count != resultCounts.end() && count->second > 1) {
        recordFailure(uniqueSubmissionAttemptIds[index], {},
                      i18n::tr("ir.upload.ambiguous_outcomes.message"));
      } else if (item != resultItems.end()) {
        std::string diagnostic =
            ir::sanitizeDiagnostic(item->second->diagnostic);
        if (diagnostic.empty()) {
          diagnostic = ir::sanitizeDiagnostic(batch->diagnostic);
        }
        recordFailure(uniqueSubmissionAttemptIds[index], diagnostic,
                      i18n::tr("ir.upload.enqueue_rejected.message"));
      } else {
        recordFailure(uniqueSubmissionAttemptIds[index], batch->diagnostic,
                      i18n::tr("ir.upload.no_outcome.message"));
      }
    }
    (void)detail::eraseQueuedAttemptIds(outcome.failedAttemptIds,
                                        outcome.queuedAttemptIds);
    return outcome;
  } catch (const std::exception &) {
    for (const std::string &attemptId : outcome.failedAttemptIds) {
      recordFailure(attemptId, {}, i18n::tr("ir.upload.ir_upload_preparation_failed.message"));
    }
    return outcome;
  } catch (...) {
    for (const std::string &attemptId : outcome.failedAttemptIds) {
      recordFailure(attemptId, {}, i18n::tr("ir.upload.ir_upload_preparation_failed.message"));
    }
    return outcome;
  }
}

void Controller::replaceCandidates(
    std::vector<ir::IrUploadCandidate> candidates) {
  candidates_ = std::move(candidates);
  ir::detail::intersectIrUploadSelectionIndexed(selectedAttemptIds_,
                                                candidates_);
  applySessionFailureReasons();
}

void Controller::applySessionFailureReasons() {
  std::unordered_set<std::string> publishedAttemptIds;
  publishedAttemptIds.reserve(candidates_.size());
  for (auto &candidate : candidates_) {
    publishedAttemptIds.insert(candidate.result.attemptId);
    const auto found =
        sessionFailureReasons_.find(candidate.result.attemptId);
    if (found != sessionFailureReasons_.end()) {
      candidate.failureReason = found->second;
    }
  }
  std::erase_if(sessionFailureReasons_, [&](const auto &entry) {
    return !publishedAttemptIds.contains(entry.first);
  });
}

void Controller::applyCandidateRefresh(
    std::optional<std::vector<ir::IrUploadCandidate>> candidates) {
  if (candidates.has_value()) {
    replaceCandidates(std::move(*candidates));
  }
}

void Controller::toggle(const std::string &attemptId) {
  if (preparing_) {
    return;
  }
  const auto found = std::ranges::find_if(
      candidates_, [&attemptId](const ir::IrUploadCandidate &candidate) {
        return candidate.attemptId() == attemptId;
      });
  if (found == candidates_.end()) {
    return;
  }
  if (!selectedAttemptIds_.erase(attemptId)) {
    selectedAttemptIds_.insert(attemptId);
  }
}

void Controller::selectAll() {
  if (preparing_) {
    return;
  }
  selectedAttemptIds_.reserve(candidates_.size());
  for (const auto &candidate : candidates_) {
    selectedAttemptIds_.insert(candidate.result.attemptId);
  }
}

void Controller::clearSelection() {
  if (!preparing_) {
    selectedAttemptIds_.clear();
  }
}

std::vector<ir::IrUploadCandidate> Controller::beginPreparation() {
  if (preparing_ || selectedAttemptIds_.empty()) {
    return {};
  }
  std::vector<ir::IrUploadCandidate> snapshot;
  snapshot.reserve(selectedAttemptIds_.size());
  for (const auto &candidate : candidates_) {
    if (selectedAttemptIds_.contains(candidate.result.attemptId)) {
      snapshot.push_back(candidate);
    }
  }
  if (!snapshot.empty()) {
    preparing_ = true;
    statusText_ = i18n::message("ir.upload.preparing.count",
        {{"completed", "0"}, {"total", std::to_string(snapshot.size())}});
  }
  return snapshot;
}

void Controller::setPreparationProgress(std::size_t completed,
                                        std::size_t total) {
  if (!preparing_) {
    return;
  }
  completed = std::min(completed, total);
  statusText_ = i18n::message("ir.upload.preparing.count",
      {{"completed", std::to_string(completed)}, {"total", std::to_string(total)}});
}

void Controller::markCancellationRequested() {
  if (preparing_) {
    statusText_ = i18n::message("ir.upload.cancelling.progress");
  }
}

void Controller::completePreparation(const PreparationOutcome &outcome) {
  if (!preparing_) {
    return;
  }
  selectedAttemptIds_.clear();
  selectedAttemptIds_.insert(outcome.failedAttemptIds.begin(),
                             outcome.failedAttemptIds.end());
  preparing_ = false;
  if (outcome.cancelled) {
    statusText_ = i18n::message("ir.upload.upload_cancelled.message");
    return;
  }
  std::unordered_set<std::string> queuedAttemptIds;
  queuedAttemptIds.reserve(outcome.queuedAttemptIds.size());
  queuedAttemptIds.insert(outcome.queuedAttemptIds.begin(),
                          outcome.queuedAttemptIds.end());
  for (const std::string &attemptId : queuedAttemptIds) {
    sessionFailureReasons_.erase(attemptId);
  }
  std::unordered_set<std::string> failedAttemptIds;
  failedAttemptIds.reserve(outcome.failedAttemptIds.size());
  failedAttemptIds.insert(outcome.failedAttemptIds.begin(),
                          outcome.failedAttemptIds.end());
  for (const auto &failure : outcome.failureReasons) {
    if (!failure.attemptId.empty() &&
        failedAttemptIds.contains(failure.attemptId)) {
      sessionFailureReasons_[failure.attemptId] = normalizedFailureReason(
          failure.diagnostic, i18n::tr("ir.upload.ir_upload_preparation_failed.message"));
    }
  }
  for (auto &candidate : candidates_) {
    if (queuedAttemptIds.contains(candidate.result.attemptId)) {
      candidate.failureReason.clear();
      continue;
    }
    const auto found =
        sessionFailureReasons_.find(candidate.result.attemptId);
    if (found != sessionFailureReasons_.end()) {
      candidate.failureReason = found->second;
    }
  }
  statusText_ = i18n::message("ir.upload.completed.count",
      {{"queued", std::to_string(outcome.queuedAttemptIds.size())},
       {"failed", std::to_string(outcome.failedAttemptIds.size())}});
}

} // namespace ir_uploads
