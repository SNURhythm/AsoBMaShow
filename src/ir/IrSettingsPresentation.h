#pragma once

#include "IrDriver.h"
#include "IrOutboxModels.h"
#include "IrProfileSettings.h"
#include "IrSubmissionService.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace ir {

inline constexpr std::size_t kMaximumRecordSyncStatusBytes = 640;

struct IrSettingsPresentationInput {
  std::string providerId;
  std::string displayName;
  IrDriverCapabilities capabilities;
  IrProviderSettings settings;
  bool hasCredential = false;
  bool serviceActive = false;
  IrOutboxCounts counts;
  IrReconciliationStatusSnapshot reconciliationStatus;
  std::chrono::steady_clock::time_point now{};
};

struct IrSettingsPresentation {
  std::string providerId;
  std::string displayName;
  bool readOnly = false;
  bool enabled = false;
  bool autoSubmit = false;
  bool hasCredential = false;
  bool showAutoSubmit = false;
  bool showQueueActions = false;
  bool canRetryAll = false;
  bool canDiscard = false;
  bool showRecordSync = false;
  bool canSyncRecords = false;
  bool insecureServerOrigin = false;
  std::string serverOrigin;
  std::string credentialLabel;
  std::string recordSyncButtonLabel;
  std::string recordSyncHelperText;
  std::string recordSyncStatusText;
  std::string recordSyncCooldownText;
  bool recordSyncStatusIsError = false;
  IrOutboxCounts counts;
};

[[nodiscard]] IrSettingsPresentation
makeIrSettingsPresentation(IrSettingsPresentationInput input);

struct IrSettingsActionResult {
  enum class Status { Succeeded, Unsupported, Invalid, StorageFailure };

  Status status = Status::StorageFailure;
  std::string diagnostic;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == Status::Succeeded;
  }
};

struct IrSettingsActionDependencies {
  std::function<bool(const IrProviderSettings &candidate,
                     std::string &diagnostic)>
      storeSettings;
  std::function<void(const IrProviderSettings &candidate)> settingsCommitted;
  std::function<bool(std::string &diagnostic)> quiesceRemoteWork;
  std::function<bool(std::string_view providerId, std::string &diagnostic)>
      invalidateProviderIdentity;
  std::function<bool(std::string_view apiKey, std::string &diagnostic)>
      replaceCredential;
  std::function<bool(std::string &diagnostic)> removeCredential;
  std::function<void()> credentialCommitted;
  std::function<bool(std::string &diagnostic)> reactivateRemoteWork;
  std::function<IrOutboxMutationOutcome()> retryAll;
  std::function<IrOutboxMutationOutcome(std::int64_t rowId)> discard;
};

class IrSettingsActionModel {
public:
  IrSettingsActionModel(std::string providerId,
                        IrDriverCapabilities capabilities,
                        IrProviderSettings settings, bool hasCredential,
                        IrSettingsActionDependencies dependencies);

  [[nodiscard]] const IrProviderSettings &settings() const noexcept;
  [[nodiscard]] bool hasCredential() const noexcept;
  [[nodiscard]] bool
  observeReconciliationRevision(std::uint64_t revision) noexcept;
  [[nodiscard]] bool observeReconciliationCooldown(bool active) noexcept;

  [[nodiscard]] IrSettingsActionResult setEnabled(bool enabled);
  [[nodiscard]] IrSettingsActionResult setAutoSubmit(bool autoSubmit);
  [[nodiscard]] IrSettingsActionResult
  setServerOrigin(std::string_view serverOrigin);
  [[nodiscard]] IrSettingsActionResult
  replaceCredential(std::string_view apiKey);
  [[nodiscard]] IrSettingsActionResult removeCredential();
  [[nodiscard]] IrSettingsActionResult retryAll();
  [[nodiscard]] IrSettingsActionResult discard(std::int64_t rowId);

private:
  [[nodiscard]] IrSettingsActionResult
  commitSettings(IrProviderSettings candidate);
  [[nodiscard]] bool supportsSubmissionActions() const noexcept;

  std::string providerId_;
  IrDriverCapabilities capabilities_;
  IrProviderSettings settings_;
  bool hasCredential_ = false;
  bool hasObservedReconciliationRevision_ = false;
  std::uint64_t observedReconciliationRevision_ = 0;
  bool hasObservedReconciliationCooldown_ = false;
  bool observedReconciliationCooldownActive_ = false;
  IrSettingsActionDependencies dependencies_;
};

} // namespace ir
