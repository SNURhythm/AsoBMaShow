#include "IrSettingsPresentation.h"

#include "IrCredentialStore.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace ir {
namespace {

IrSettingsActionResult unsupported(std::string diagnostic) {
  return {.status = IrSettingsActionResult::Status::Unsupported,
          .diagnostic = std::move(diagnostic)};
}

IrSettingsActionResult mutationResult(const IrOutboxMutationOutcome &outcome) {
  if (outcome.status == IrOutboxMutationStatus::Updated) {
    return {.status = IrSettingsActionResult::Status::Succeeded};
  }
  return {.status = IrSettingsActionResult::Status::StorageFailure,
          .diagnostic = sanitizeDiagnostic(outcome.diagnostic)};
}

bool recordSyncIsRunning(IrReconciliationPhase phase) noexcept {
  return phase == IrReconciliationPhase::Queued ||
         phase == IrReconciliationPhase::Fetching7K ||
         phase == IrReconciliationPhase::Fetching14K ||
         phase == IrReconciliationPhase::Applying;
}

std::string
recordSyncMutationSummary(const IrReconciliationStatusSnapshot &status) {
  return "Sync complete. Records: " +
         std::to_string(std::max(0, status.remoteScores)) + " total, " +
         std::to_string(std::max(0, status.remoteScoresAdded)) + " added, " +
         std::to_string(std::max(0, status.remoteScoresRemoved)) +
         " removed. Receipts: " +
         std::to_string(std::max(0, status.receiptsUpserted)) + " confirmed, " +
         std::to_string(std::max(0, status.receiptsDeleted)) + " removed, " +
         std::to_string(std::max(0, status.ambiguousReceiptsPreserved)) +
         " ambiguous. Settled outbox rows: " +
         std::to_string(std::max(0, status.outboxRowsSettled)) + ".";
}

std::string recordSyncFailureSummary(std::string_view diagnostic) {
  const std::string bounded = sanitizeDiagnostic(diagnostic);
  if (bounded.empty()) {
    return "Sync failed. Existing records and receipts were left unchanged.";
  }
  return "Sync failed: " + bounded +
         " Existing records and receipts were left unchanged.";
}

std::string recordSyncStatusText(const IrReconciliationStatusSnapshot &status,
                                 bool cooldownActive) {
  switch (status.phase) {
  case IrReconciliationPhase::Idle:
    return "Ready to import remote records and reconcile upload receipts.";
  case IrReconciliationPhase::Queued:
    return "Sync queued. Waiting for active uploads to finish.";
  case IrReconciliationPhase::Fetching7K:
    return "Request 1 of 2: fetching 7K records.";
  case IrReconciliationPhase::Fetching14K:
    return "Request 2 of 2: fetching 14K records.";
  case IrReconciliationPhase::Applying:
    return "Both requests validated. Applying records and receipts atomically.";
  case IrReconciliationPhase::Succeeded:
    return recordSyncMutationSummary(status);
  case IrReconciliationPhase::Failed:
    return recordSyncFailureSummary(status.diagnostic);
  case IrReconciliationPhase::Cooldown:
    return cooldownActive ? "Sync cooldown is active."
                          : "Sync cooldown complete. Record sync is available.";
  }
  return "Record sync status is unavailable.";
}

std::string recordSyncCooldownText(const IrReconciliationStatusSnapshot &status,
                                   std::chrono::steady_clock::time_point now) {
  if (!status.nextAllowedAt || now >= *status.nextAllowedAt) {
    return {};
  }
  const auto remaining = *status.nextAllowedAt - now;
  auto seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);
  if (seconds < remaining) {
    seconds += std::chrono::seconds{1};
  }
  return "Available again in " + std::to_string(seconds.count()) +
         (seconds == std::chrono::seconds{1} ? " second." : " seconds.");
}

class RemoteWorkReactivationGuard {
public:
  explicit RemoteWorkReactivationGuard(
      std::function<bool(std::string &)> callback)
      : callback_(std::move(callback)) {}

  ~RemoteWorkReactivationGuard() {
    if (!attempted_) {
      std::string ignored;
      (void)reactivate(ignored);
    }
  }

  bool reactivate(std::string &diagnostic) noexcept {
    if (attempted_) {
      return succeeded_;
    }
    attempted_ = true;
    try {
      succeeded_ = callback_ && callback_(diagnostic);
    } catch (...) {
      diagnostic = "IR account work could not be reactivated.";
      succeeded_ = false;
    }
    return succeeded_;
  }

private:
  std::function<bool(std::string &)> callback_;
  bool attempted_ = false;
  bool succeeded_ = false;
};

} // namespace

IrSettingsPresentation
makeIrSettingsPresentation(IrSettingsPresentationInput input) {
  sanitizeProviderSettings(input.settings);
  const bool supportsSubmission =
      !input.capabilities.readOnly && input.capabilities.scoreSubmission;
  const bool supportsQueue =
      supportsSubmission && input.capabilities.deferredSubmission;
  const bool supportsRecordSync = input.capabilities.scoreReconciliation &&
                                  input.settings.enabled &&
                                  input.hasCredential && input.serviceActive;
  const bool cooldownActive =
      input.reconciliationStatus.nextAllowedAt &&
      input.now < *input.reconciliationStatus.nextAllowedAt;
  const bool recordSyncRunning =
      recordSyncIsRunning(input.reconciliationStatus.phase);
  std::string syncStatus =
      recordSyncStatusText(input.reconciliationStatus, cooldownActive);
  if (syncStatus.size() > kMaximumRecordSyncStatusBytes) {
    syncStatus.resize(kMaximumRecordSyncStatusBytes);
  }
  return {
      .providerId = std::move(input.providerId),
      .displayName = std::move(input.displayName),
      .readOnly = input.capabilities.readOnly,
      .enabled = input.settings.enabled,
      .autoSubmit = supportsSubmission && input.settings.autoSubmit,
      .hasCredential = input.hasCredential,
      .showAutoSubmit = supportsSubmission,
      .showQueueActions = supportsQueue,
      .canRetryAll =
          supportsQueue && input.counts.storageAvailable &&
          (input.counts.pending > 0 || input.counts.awaitingRemoteResult > 0 ||
           input.counts.failedPermanent > 0 ||
           input.counts.blockedConfiguration > 0),
      .canDiscard =
          supportsQueue && input.counts.storageAvailable &&
          (input.counts.pending > 0 || input.counts.awaitingRemoteResult > 0 ||
           input.counts.blockedConfiguration > 0 ||
           input.counts.failedPermanent > 0),
      .showRecordSync = supportsRecordSync,
      .canSyncRecords =
          supportsRecordSync && !recordSyncRunning && !cooldownActive,
      .insecureServerOrigin =
          input.settings.serverOrigin.starts_with("http://"),
      .serverOrigin = std::move(input.settings.serverOrigin),
      .credentialLabel =
          input.hasCredential ? "API key saved (••••••••)" : "No API key saved",
      .recordSyncButtonLabel = "Import & Reconcile",
      .recordSyncHelperText =
          "Uses exactly two requests (7K, then 14K) to import remote score "
          "history and reconcile existing upload receipts. Local scores are "
          "not uploaded.",
      .recordSyncStatusText = std::move(syncStatus),
      .recordSyncCooldownText =
          recordSyncCooldownText(input.reconciliationStatus, input.now),
      .recordSyncStatusIsError =
          input.reconciliationStatus.phase == IrReconciliationPhase::Failed,
      .counts = std::move(input.counts),
  };
}

IrSettingsActionModel::IrSettingsActionModel(
    std::string providerId, IrDriverCapabilities capabilities,
    IrProviderSettings settings, bool hasCredential,
    IrSettingsActionDependencies dependencies)
    : providerId_(std::move(providerId)), capabilities_(capabilities),
      settings_(std::move(settings)), hasCredential_(hasCredential),
      dependencies_(std::move(dependencies)) {
  sanitizeProviderSettings(settings_);
  if (!supportsSubmissionActions()) {
    settings_.autoSubmit = false;
  }
}

const IrProviderSettings &IrSettingsActionModel::settings() const noexcept {
  return settings_;
}

bool IrSettingsActionModel::hasCredential() const noexcept {
  return hasCredential_;
}

bool IrSettingsActionModel::observeReconciliationRevision(
    std::uint64_t revision) noexcept {
  const bool changed = !hasObservedReconciliationRevision_ ||
                       revision != observedReconciliationRevision_;
  hasObservedReconciliationRevision_ = true;
  observedReconciliationRevision_ = revision;
  return changed;
}

bool IrSettingsActionModel::observeReconciliationCooldown(
    bool active) noexcept {
  const bool changed = !hasObservedReconciliationCooldown_ ||
                       active != observedReconciliationCooldownActive_;
  hasObservedReconciliationCooldown_ = true;
  observedReconciliationCooldownActive_ = active;
  return changed;
}

IrSettingsActionResult IrSettingsActionModel::setEnabled(bool enabled) {
  IrProviderSettings candidate = settings_;
  candidate.enabled = enabled;
  return commitSettings(std::move(candidate));
}

IrSettingsActionResult IrSettingsActionModel::setAutoSubmit(bool autoSubmit) {
  if (!supportsSubmissionActions()) {
    return unsupported("This IR provider is read-only.");
  }
  IrProviderSettings candidate = settings_;
  candidate.autoSubmit = autoSubmit;
  return commitSettings(std::move(candidate));
}

IrSettingsActionResult
IrSettingsActionModel::setServerOrigin(std::string_view serverOrigin) {
  const auto normalized = normalizeServerOrigin(serverOrigin);
  if (!normalized.has_value()) {
    return {.status = IrSettingsActionResult::Status::Invalid,
            .diagnostic = "Enter an HTTP or HTTPS server origin."};
  }
  IrProviderSettings candidate = settings_;
  candidate.serverOrigin = *normalized;
  return commitSettings(std::move(candidate));
}

IrSettingsActionResult
IrSettingsActionModel::replaceCredential(std::string_view apiKey) {
  if (!IrCredentialStore::isApiKeyFormatValid(apiKey)) {
    return {.status = IrSettingsActionResult::Status::Invalid,
            .diagnostic = "Enter a valid API key."};
  }
  if (!dependencies_.replaceCredential) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "API key storage is unavailable."};
  }
  if (!dependencies_.quiesceRemoteWork ||
      !dependencies_.invalidateProviderIdentity ||
      !dependencies_.reactivateRemoteWork) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "IR account mutation isolation is unavailable."};
  }
  std::string ignoredDiagnostic;
  RemoteWorkReactivationGuard reactivation(
      dependencies_.reactivateRemoteWork);
  try {
    if (!dependencies_.quiesceRemoteWork(ignoredDiagnostic)) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = "IR account work could not be paused."};
    }
  } catch (...) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "IR account work could not be paused."};
  }
  try {
    if (!dependencies_.replaceCredential(apiKey, ignoredDiagnostic)) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = "API key could not be saved."};
    }
  } catch (...) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "API key could not be saved."};
  }
  hasCredential_ = true;
  try {
    if (!dependencies_.invalidateProviderIdentity(providerId_,
                                                  ignoredDiagnostic)) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = "IR account evidence could not be invalidated."};
    }
  } catch (...) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "IR account evidence could not be invalidated."};
  }
  if (dependencies_.credentialCommitted) {
    try {
      dependencies_.credentialCommitted();
    } catch (...) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = "The saved API key could not be activated."};
    }
  }
  if (!reactivation.reactivate(ignoredDiagnostic)) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "IR account work could not be reactivated."};
  }
  return {.status = IrSettingsActionResult::Status::Succeeded};
}

IrSettingsActionResult IrSettingsActionModel::removeCredential() {
  if (!dependencies_.removeCredential) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "API key storage is unavailable."};
  }
  if (!dependencies_.quiesceRemoteWork ||
      !dependencies_.invalidateProviderIdentity ||
      !dependencies_.reactivateRemoteWork) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "IR account mutation isolation is unavailable."};
  }
  std::string ignoredDiagnostic;
  RemoteWorkReactivationGuard reactivation(
      dependencies_.reactivateRemoteWork);
  try {
    if (!dependencies_.quiesceRemoteWork(ignoredDiagnostic)) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = "IR account work could not be paused."};
    }
  } catch (...) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "IR account work could not be paused."};
  }
  try {
    if (!dependencies_.invalidateProviderIdentity(providerId_,
                                                   ignoredDiagnostic)) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic =
                  "IR account evidence could not be invalidated."};
    }
  } catch (...) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "IR account evidence could not be invalidated."};
  }
  try {
    if (!dependencies_.removeCredential(ignoredDiagnostic)) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = "API key could not be removed."};
    }
  } catch (...) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "API key could not be removed."};
  }
  hasCredential_ = false;
  if (dependencies_.credentialCommitted) {
    try {
      dependencies_.credentialCommitted();
    } catch (...) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = "The removed API key could not be activated."};
    }
  }
  if (!reactivation.reactivate(ignoredDiagnostic)) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "IR account work could not be reactivated."};
  }
  return {.status = IrSettingsActionResult::Status::Succeeded};
}

IrSettingsActionResult IrSettingsActionModel::retryAll() {
  if (!supportsSubmissionActions() || !capabilities_.deferredSubmission) {
    return unsupported("This IR provider has no submission queue.");
  }
  if (!dependencies_.retryAll) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "The submission queue is unavailable."};
  }
  return mutationResult(dependencies_.retryAll());
}

IrSettingsActionResult IrSettingsActionModel::discard(std::int64_t rowId) {
  if (!supportsSubmissionActions() || !capabilities_.deferredSubmission) {
    return unsupported("This IR provider has no submission queue.");
  }
  if (rowId <= 0) {
    return {.status = IrSettingsActionResult::Status::Invalid,
            .diagnostic = "Select a queued submission to discard."};
  }
  if (!dependencies_.discard) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "The submission queue is unavailable."};
  }
  return mutationResult(dependencies_.discard(rowId));
}

IrSettingsActionResult
IrSettingsActionModel::commitSettings(IrProviderSettings candidate) {
  sanitizeProviderSettings(candidate);
  if (candidate == settings_) {
    return {.status = IrSettingsActionResult::Status::Succeeded};
  }
  if (!dependencies_.storeSettings) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "IR settings storage is unavailable."};
  }
  std::string diagnostic;
  if (!dependencies_.storeSettings(candidate, diagnostic)) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = sanitizeDiagnostic(diagnostic)};
  }
  settings_ = std::move(candidate);
  if (dependencies_.settingsCommitted) {
    dependencies_.settingsCommitted(settings_);
  }
  return {.status = IrSettingsActionResult::Status::Succeeded};
}

bool IrSettingsActionModel::supportsSubmissionActions() const noexcept {
  return !capabilities_.readOnly && capabilities_.scoreSubmission;
}

} // namespace ir
