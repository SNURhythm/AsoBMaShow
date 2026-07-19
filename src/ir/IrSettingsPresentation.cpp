#include "IrSettingsPresentation.h"

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
      .insecureServerOrigin =
          input.settings.serverOrigin.starts_with("http://"),
      .serverOrigin = std::move(input.settings.serverOrigin),
      .credentialLabel =
          input.hasCredential ? "API key saved (••••••••)" : "No API key saved",
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
  if (apiKey.empty() || apiKey.size() > 4U * 1024U) {
    return {.status = IrSettingsActionResult::Status::Invalid,
            .diagnostic = "Enter a non-empty API key."};
  }
  if (!dependencies_.replaceCredential) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "API key storage is unavailable."};
  }
  if (!dependencies_.quiesceRemoteWork ||
      !dependencies_.invalidateRemoteIdentity ||
      !dependencies_.reactivateRemoteWork) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "IR account mutation isolation is unavailable."};
  }
  const auto origin = normalizeServerOrigin(settings_.serverOrigin);
  if (!origin) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "The current IR server origin is invalid."};
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
    if (!dependencies_.invalidateRemoteIdentity(providerId_, *origin,
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
    if (!dependencies_.replaceCredential(apiKey, ignoredDiagnostic)) {
      return {.status = IrSettingsActionResult::Status::StorageFailure,
              .diagnostic = "API key could not be saved."};
    }
  } catch (...) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "API key could not be saved."};
  }
  hasCredential_ = true;
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
      !dependencies_.invalidateRemoteIdentity ||
      !dependencies_.reactivateRemoteWork) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "IR account mutation isolation is unavailable."};
  }
  const auto origin = normalizeServerOrigin(settings_.serverOrigin);
  if (!origin) {
    return {.status = IrSettingsActionResult::Status::StorageFailure,
            .diagnostic = "The current IR server origin is invalid."};
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
    if (!dependencies_.invalidateRemoteIdentity(providerId_, *origin,
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
