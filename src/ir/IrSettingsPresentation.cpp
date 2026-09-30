#include "../i18n/Localization.h"
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
    return i18n::tr("settings.ir.sync_failed_existing_records_receipts_left_unchanged.message");
  }
  return i18n::tr("settings.ir.sync_failed.prefix") + bounded +
         i18n::tr("settings.ir.existing_records_receipts_left_unchanged.suffix");
}

std::string recordSyncStatusText(const IrReconciliationStatusSnapshot &status,
                                 bool cooldownActive) {
  switch (status.phase) {
  case IrReconciliationPhase::Idle:
    return i18n::tr("settings.ir.ready_import_remote_records_reconcile_upload_receipts.message");
  case IrReconciliationPhase::Queued:
    return i18n::tr("settings.ir.sync_queued_waiting_active_uploads_finish.message");
  case IrReconciliationPhase::Fetching7K:
    return i18n::tr("settings.ir.request_1_2_fetching_7_k_records.message");
  case IrReconciliationPhase::Fetching14K:
    return i18n::tr("settings.ir.request_2_2_fetching_14_k_records.message");
  case IrReconciliationPhase::Applying:
    return i18n::tr("settings.ir.remote_history.applying_status");
  case IrReconciliationPhase::Succeeded:
    return recordSyncMutationSummary(status);
  case IrReconciliationPhase::Failed:
    return recordSyncFailureSummary(status.diagnostic);
  case IrReconciliationPhase::Cooldown:
    return cooldownActive ? i18n::tr("settings.ir.sync_cooldown_active.message")
                          : i18n::tr("settings.ir.sync_cooldown_complete_record_sync_available.message");
  }
  return i18n::tr("settings.ir.record_sync_status_unavailable.message");
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
  return i18n::format(seconds == std::chrono::seconds{1}
                          ? "settings.ir.remote_history.cooldown.one"
                          : "settings.ir.remote_history.cooldown.other",
                      {{"seconds", std::to_string(seconds.count())}});
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
      diagnostic = i18n::tr("settings.ir.ir_account_work_failed_reactivated.message");
      succeeded_ = false;
    }
    return succeeded_;
  }

  void leavePaused() noexcept {
    attempted_ = true;
    succeeded_ = false;
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
  const bool secureServerOrigin =
      isHttpsServerOrigin(input.settings.serverOrigin);
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
      .autoSubmit = supportsSubmission && secureServerOrigin &&
                    input.settings.autoSubmit,
      .hasCredential = input.hasCredential,
      .showAutoSubmit = supportsSubmission,
      .showQueueActions = supportsQueue,
      .canRetryAll =
          supportsQueue && secureServerOrigin &&
          input.counts.storageAvailable &&
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
          supportsRecordSync && secureServerOrigin && !recordSyncRunning &&
          !cooldownActive,
      .authenticatedActionsAvailable = secureServerOrigin,
      .insecureServerOrigin =
          input.settings.serverOrigin.starts_with("http://"),
      .serverOrigin = std::move(input.settings.serverOrigin),
      .credentialLabel =
          input.hasCredential ? i18n::tr("settings.ir.api_key_saved.label") : i18n::tr("settings.ir.no_api_key_saved.label"),
      .recordSyncButtonLabel = i18n::tr("settings.ir.import_reconcile.label"),
      .recordSyncHelperText =
          i18n::tr("settings.ir.remote_history.sync_description"),
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
    return unsupported(i18n::tr("settings.ir.ir_provider_read_only.message"));
  }
  if (autoSubmit && !isHttpsServerOrigin(settings_.serverOrigin)) {
    return {.status = IrSettingsActionResult::Status::Invalid,
            .diagnostic =
                i18n::tr("settings.ir.use_https_server_origin_before_enabling_submissions.message")};
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
            .diagnostic = i18n::tr("settings.ir.enter_http_https_server_origin.message")};
  }
  if (*normalized != settings_.serverOrigin) {
    std::optional<std::string> credential;
    std::string ignoredDiagnostic;
    bool credentialLoaded = false;
    try {
      credentialLoaded = dependencies_.loadCredential &&
                         dependencies_.loadCredential(credential,
                                                      ignoredDiagnostic);
    } catch (...) {
      credentialLoaded = false;
    }
    if (!credentialLoaded) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = i18n::tr("settings.ir.saved_api_key_failed_checked_server_origin_not_changed.message")};
    }
    hasCredential_ = credential.has_value();
    if (hasCredential_) {
      return {.status = IrSettingsActionResult::Status::Invalid,
              .diagnostic = i18n::tr("settings.ir.server_origin.credential_removal_required")};
    }
  }
  IrProviderSettings candidate = settings_;
  candidate.serverOrigin = *normalized;
  if (!isHttpsServerOrigin(candidate.serverOrigin)) {
    candidate.autoSubmit = false;
  }
  return commitSettings(std::move(candidate));
}

IrSettingsActionResult
IrSettingsActionModel::replaceCredential(std::string_view apiKey) {
  if (!IrCredentialStore::isApiKeyFormatValid(apiKey)) {
    return {.status = IrSettingsActionResult::Status::Invalid,
            .diagnostic = i18n::tr("settings.ir.enter_valid_api_key.message")};
  }
  if (!isHttpsServerOrigin(settings_.serverOrigin)) {
    return {.status = IrSettingsActionResult::Status::Invalid,
            .diagnostic =
                i18n::tr("settings.ir.use_https_server_origin_before_saving_api_key.message")};
  }
  if (!dependencies_.replaceCredential) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.api_key_storage_unavailable.message")};
  }
  if (!dependencies_.quiesceRemoteWork || !dependencies_.loadCredential ||
      !dependencies_.invalidateProviderIdentity ||
      !dependencies_.removeCredential ||
      !dependencies_.reactivateRemoteWork) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.ir_account_mutation_isolation_unavailable.message")};
  }
  std::string ignoredDiagnostic;
  RemoteWorkReactivationGuard reactivation(
      dependencies_.reactivateRemoteWork);
  try {
    if (!dependencies_.quiesceRemoteWork(ignoredDiagnostic)) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = i18n::tr("settings.ir.ir_account_work_failed_paused.message")};
    }
  } catch (...) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.ir_account_work_failed_paused.message")};
  }
  std::optional<std::string> previousCredential;
  try {
    if (!dependencies_.loadCredential(previousCredential,
                                      ignoredDiagnostic) ||
        (previousCredential && !IrCredentialStore::isApiKeyFormatValid(
                                   *previousCredential))) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = i18n::tr("settings.ir.existing_api_key_failed_read.message")};
    }
  } catch (...) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.existing_api_key_failed_read.message")};
  }
  if (previousCredential && *previousCredential == apiKey) {
    hasCredential_ = true;
    if (!reactivation.reactivate(ignoredDiagnostic)) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = i18n::tr("settings.ir.ir_account_work_failed_reactivated.message")};
    }
    return {.status = IrSettingsActionResult::Status::Succeeded};
  }
  try {
    if (!dependencies_.replaceCredential(apiKey, ignoredDiagnostic)) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = i18n::tr("settings.ir.api_key_failed_saved.message")};
    }
  } catch (...) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.api_key_failed_saved.message")};
  }
  bool identityInvalidated = false;
  try {
    identityInvalidated = dependencies_.invalidateProviderIdentity(
        providerId_, ignoredDiagnostic);
  } catch (...) {
    identityInvalidated = false;
  }
  if (!identityInvalidated) {
    bool rolledBack = false;
    try {
      rolledBack = previousCredential
                       ? dependencies_.replaceCredential(*previousCredential,
                                                         ignoredDiagnostic)
                       : dependencies_.removeCredential(ignoredDiagnostic);
    } catch (...) {
      rolledBack = false;
    }
    if (!rolledBack) {
      hasCredential_ = true;
      reactivation.leavePaused();
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic =
                  i18n::tr("settings.ir.credentials.replace_rollback_failed")};
    }
    hasCredential_ = previousCredential.has_value();
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.ir_account_evidence_failed_invalidated.message")};
  }
  hasCredential_ = true;
  if (dependencies_.credentialCommitted) {
    try {
      dependencies_.credentialCommitted();
    } catch (...) {
      reactivation.leavePaused();
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = i18n::tr("settings.ir.saved_api_key_failed_activated_ir_work_remains_paused.message")};
    }
  }
  if (!reactivation.reactivate(ignoredDiagnostic)) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.ir_account_work_failed_reactivated.message")};
  }
  return {.status = IrSettingsActionResult::Status::Succeeded};
}

IrSettingsActionResult IrSettingsActionModel::removeCredential() {
  if (!dependencies_.removeCredential) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.api_key_storage_unavailable.message")};
  }
  if (!dependencies_.quiesceRemoteWork || !dependencies_.loadCredential ||
      !dependencies_.invalidateProviderIdentity ||
      !dependencies_.replaceCredential ||
      !dependencies_.reactivateRemoteWork) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.ir_account_mutation_isolation_unavailable.message")};
  }
  std::string ignoredDiagnostic;
  RemoteWorkReactivationGuard reactivation(
      dependencies_.reactivateRemoteWork);
  try {
    if (!dependencies_.quiesceRemoteWork(ignoredDiagnostic)) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = i18n::tr("settings.ir.ir_account_work_failed_paused.message")};
    }
  } catch (...) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.ir_account_work_failed_paused.message")};
  }
  std::optional<std::string> previousCredential;
  try {
    if (!dependencies_.loadCredential(previousCredential,
                                      ignoredDiagnostic) ||
        (previousCredential && !IrCredentialStore::isApiKeyFormatValid(
                                   *previousCredential))) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = i18n::tr("settings.ir.existing_api_key_failed_read.message")};
    }
  } catch (...) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.existing_api_key_failed_read.message")};
  }
  try {
    if (!dependencies_.removeCredential(ignoredDiagnostic)) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = i18n::tr("settings.ir.api_key_failed_removed.message")};
    }
  } catch (...) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.api_key_failed_removed.message")};
  }
  bool identityInvalidated = false;
  try {
    identityInvalidated = dependencies_.invalidateProviderIdentity(
        providerId_, ignoredDiagnostic);
  } catch (...) {
    identityInvalidated = false;
  }
  if (!identityInvalidated) {
    bool rolledBack = !previousCredential;
    if (previousCredential) {
      try {
        rolledBack = dependencies_.replaceCredential(*previousCredential,
                                                       ignoredDiagnostic);
      } catch (...) {
        rolledBack = false;
      }
    }
    if (!rolledBack) {
      hasCredential_ = false;
      reactivation.leavePaused();
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic =
                  i18n::tr("settings.ir.credentials.remove_rollback_failed")};
    }
    hasCredential_ = previousCredential.has_value();
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.ir_account_evidence_failed_invalidated.message")};
  }
  hasCredential_ = false;
  if (dependencies_.credentialCommitted) {
    try {
      dependencies_.credentialCommitted();
    } catch (...) {
      reactivation.leavePaused();
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = i18n::tr("settings.ir.removed_api_key_failed_activated_ir_work_remains_paused.message")};
    }
  }
  if (!reactivation.reactivate(ignoredDiagnostic)) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.ir_account_work_failed_reactivated.message")};
  }
  return {.status = IrSettingsActionResult::Status::Succeeded};
}

IrSettingsActionResult IrSettingsActionModel::retryAll() {
  if (!supportsSubmissionActions() || !capabilities_.deferredSubmission) {
    return unsupported(i18n::tr("settings.ir.ir_provider_has_no_submission_queue.message"));
  }
  if (!isHttpsServerOrigin(settings_.serverOrigin)) {
    return {.status = IrSettingsActionResult::Status::Invalid,
            .diagnostic =
                i18n::tr("settings.ir.use_https_server_origin_before_retrying_submissions.message")};
  }
  if (!dependencies_.retryAll) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.submission_queue_unavailable.message")};
  }
  return mutationResult(dependencies_.retryAll());
}

IrSettingsActionResult IrSettingsActionModel::discard(std::int64_t rowId) {
  if (!supportsSubmissionActions() || !capabilities_.deferredSubmission) {
    return unsupported(i18n::tr("settings.ir.ir_provider_has_no_submission_queue.message"));
  }
  if (rowId <= 0) {
    return {.status = IrSettingsActionResult::Status::Invalid,
            .diagnostic = i18n::tr("settings.ir.select_queued_submission_discard.message")};
  }
  if (!dependencies_.discard) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = i18n::tr("settings.ir.submission_queue_unavailable.message")};
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
            .diagnostic = i18n::tr("settings.ir.ir_settings_storage_unavailable.message")};
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
