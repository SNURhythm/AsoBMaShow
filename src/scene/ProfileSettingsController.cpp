#include "ProfileSettingsController.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace {
ProfileResult profileFailure(ProfileError error, std::string message) {
  return {.error = error, .message = std::move(message)};
}

ProfileArchiveResult archiveFailure(ProfileError error, std::string message) {
  return {.error = error, .message = std::move(message)};
}

ProfileSwitchResult switchFailure(ProfileError error, std::string message) {
  return {.error = error, .message = std::move(message)};
}

std::string exceptionMessage(const std::exception &error,
                             std::string_view operation) {
  return std::string(operation) + ": " + error.what();
}
} // namespace

ProfileSettingsController::ProfileSettingsController(
    ProfileSettingsControllerDependencies dependencies)
    : dependencies_(std::move(dependencies)) {
  refresh();
}

ProfileSettingsController::~ProfileSettingsController() {
  releaseArchivePipeline();
}

const std::vector<PlayerProfile> &ProfileSettingsController::profiles() const {
  return profiles_;
}

const std::string &ProfileSettingsController::activeProfileId() const {
  return activeProfileId_;
}

const std::string &ProfileSettingsController::selectedProfileId() const {
  return selectedProfileId_;
}

const std::string &ProfileSettingsController::confirmationProfileId() const {
  return confirmationProfileId_;
}

ProfileSettingsPhase ProfileSettingsController::phase() const { return phase_; }

const ProfileSettingsStatus &ProfileSettingsController::status() const {
  return status_;
}

bool ProfileSettingsController::actionsEnabled() const {
  return phase_ == ProfileSettingsPhase::Idle;
}

bool ProfileSettingsController::contains(std::string_view profileId) const {
  return std::ranges::any_of(profiles_, [&](const PlayerProfile &profile) {
    return profile.id == profileId;
  });
}

bool ProfileSettingsController::refresh() {
  if (!dependencies_.listProfiles) {
    setFailure(ProfileError::SwitchBlocked, {},
               "Player profile services are unavailable.");
    return false;
  }
  ProfileListResult result;
  try {
    result = dependencies_.listProfiles();
  } catch (const std::exception &error) {
    setFailure(ProfileError::IoFailure,
               exceptionMessage(error, "Unable to read player profiles"), {});
    return false;
  } catch (...) {
    setFailure(ProfileError::IoFailure, {}, "Unable to read player profiles.");
    return false;
  }
  if (!result.ok()) {
    setFailure(result.error, std::move(result.message),
               "Unable to read player profiles.");
    return false;
  }
  if (result.profiles.empty()) {
    setFailure(ProfileError::IntegrityFailure, {},
               "No valid player profiles were found. The previous list was "
               "kept visible.");
    return false;
  }
  const bool activeExists =
      std::ranges::any_of(result.profiles, [&](const PlayerProfile &profile) {
        return profile.id == result.activeProfileId;
      });
  if (!activeExists) {
    setFailure(ProfileError::IntegrityFailure, {},
               "The active profile is missing from the profile list. The "
               "previous list was kept visible.");
    return false;
  }

  const std::string priorSelection = selectedProfileId_;
  profiles_ = std::move(result.profiles);
  activeProfileId_ = std::move(result.activeProfileId);
  selectedProfileId_ = !priorSelection.empty() && contains(priorSelection)
                           ? priorSelection
                           : activeProfileId_;
  if (!confirmationProfileId_.empty() && !contains(confirmationProfileId_)) {
    const auto priorStatus = archivePipelinePriorStatus_
                                 ? archivePipelinePriorStatus_
                                 : confirmationPriorStatus_;
    clearTransientPhase();
    releaseArchivePipeline();
    status_ = priorStatus.value_or(ProfileSettingsStatus{});
  }
  return true;
}

bool ProfileSettingsController::select(std::string_view profileId) {
  if (!contains(profileId)) {
    return false;
  }
  if (!confirmationProfileId_.empty() && confirmationProfileId_ != profileId) {
    const auto priorStatus = archivePipelinePriorStatus_
                                 ? archivePipelinePriorStatus_
                                 : confirmationPriorStatus_;
    clearTransientPhase();
    releaseArchivePipeline();
    status_ = priorStatus.value_or(ProfileSettingsStatus{});
  }
  selectedProfileId_ = std::string(profileId);
  return true;
}

ProfileActionEligibility ProfileSettingsController::destructiveEligibility(
    std::string_view profileId, std::string_view action) const {
  if (!contains(profileId)) {
    return {.enabled = false, .reason = "The selected profile is unavailable."};
  }
  if (profileId == activeProfileId_) {
    return {.enabled = false,
            .reason = "The active profile cannot be " + std::string(action) +
                      ". Activate another profile first."};
  }
  if (profiles_.size() <= 1) {
    return {.enabled = false,
            .reason =
                "The last profile cannot be " + std::string(action) + "."};
  }
  if (phase_ != ProfileSettingsPhase::Idle) {
    return {.enabled = false,
            .reason = "Finish the current profile action first."};
  }
  return {.enabled = true};
}

ProfileActionEligibility
ProfileSettingsController::deleteEligibility(std::string_view profileId) const {
  return destructiveEligibility(profileId, "deleted");
}

ProfileActionEligibility ProfileSettingsController::overwriteEligibility(
    std::string_view profileId) const {
  return destructiveEligibility(profileId, "overwritten");
}

ProfileResult
ProfileSettingsController::unavailableResult(std::string message) const {
  return profileFailure(ProfileError::SwitchBlocked, std::move(message));
}

ProfileArchiveResult
ProfileSettingsController::unavailableArchiveResult(std::string message) const {
  return archiveFailure(ProfileError::SwitchBlocked, std::move(message));
}

void ProfileSettingsController::setFailure(ProfileError, std::string message,
                                           std::string fallback) {
  status_ = {.kind = ProfileSettingsStatusKind::Error,
             .message =
                 message.empty() ? std::move(fallback) : std::move(message)};
}

void ProfileSettingsController::setSuccess(std::string message,
                                           std::string fallback) {
  if (message.empty()) {
    status_ = {.kind = ProfileSettingsStatusKind::Success,
               .message = std::move(fallback)};
  } else {
    status_ = {.kind = ProfileSettingsStatusKind::Warning,
               .message = std::move(message)};
  }
}

bool ProfileSettingsController::refreshAfterMutation(
    std::optional<std::string> preferredProfileId,
    const std::string &operationError) {
  const std::string priorSelection = selectedProfileId_;
  if (!refresh()) {
    if (!operationError.empty()) {
      const std::string refreshError = status_.message;
      status_ = {.kind = ProfileSettingsStatusKind::Error,
                 .message = operationError + (refreshError.empty()
                                                  ? std::string{}
                                                  : "; " + refreshError)};
    }
    return false;
  }
  if (preferredProfileId && contains(*preferredProfileId)) {
    selectedProfileId_ = *preferredProfileId;
  } else if (!priorSelection.empty() && contains(priorSelection)) {
    selectedProfileId_ = priorSelection;
  } else {
    selectedProfileId_ = activeProfileId_;
  }
  return true;
}

ProfileResult ProfileSettingsController::finishMutation(
    ProfileResult result, std::string successText,
    std::optional<std::string> preferredProfileId) {
  if (!result.ok()) {
    const std::string operationError =
        result.message.empty() ? "The profile action failed." : result.message;
    if (refreshAfterMutation(std::nullopt, operationError)) {
      setFailure(result.error, result.message, "The profile action failed.");
    }
    return result;
  }
  if (result.profile) {
    preferredProfileId = result.profile->id;
  }
  if (refreshAfterMutation(std::move(preferredProfileId))) {
    setSuccess(result.message, std::move(successText));
  }
  return result;
}

bool ProfileSettingsController::flushActiveState(std::string &errorMessage) {
  errorMessage.clear();
  if (!dependencies_.flushSettings) {
    errorMessage = "Settings persistence is unavailable.";
    return false;
  }
  try {
    if (!dependencies_.flushSettings(errorMessage)) {
      if (errorMessage.empty()) {
        errorMessage = "Unable to save the active profile settings.";
      }
      return false;
    }
  } catch (const std::exception &error) {
    errorMessage = exceptionMessage(error, "Unable to save profile settings");
    return false;
  } catch (...) {
    errorMessage = "Unable to save profile settings.";
    return false;
  }
  if (!dependencies_.flushInput) {
    errorMessage = "Input persistence is unavailable.";
    return false;
  }
  try {
    if (!dependencies_.flushInput(errorMessage)) {
      if (errorMessage.empty()) {
        errorMessage = "Unable to save the active input profile.";
      }
      return false;
    }
  } catch (const std::exception &error) {
    errorMessage = exceptionMessage(error, "Unable to save input profile");
    return false;
  } catch (...) {
    errorMessage = "Unable to save input profile.";
    return false;
  }
  return true;
}

bool ProfileSettingsController::acquireArchivePipeline() {
  if (archivePipelineHeld_) {
    return true;
  }
  if (!dependencies_.beginArchivePipeline) {
    archivePipelineHeld_ = true;
    archivePipelinePriorStatus_ = status_;
    return true;
  }
  std::string errorMessage;
  try {
    if (!dependencies_.beginArchivePipeline(errorMessage)) {
      setFailure(ProfileError::SwitchBlocked, std::move(errorMessage),
                 "Another profile archive action is active.");
      return false;
    }
  } catch (const std::exception &error) {
    setFailure(ProfileError::SwitchBlocked,
               exceptionMessage(error, "Unable to start archive action"), {});
    return false;
  } catch (...) {
    setFailure(ProfileError::SwitchBlocked, {},
               "Unable to start archive action.");
    return false;
  }
  archivePipelineHeld_ = true;
  archivePipelinePriorStatus_ = status_;
  return true;
}

void ProfileSettingsController::releaseArchivePipeline() {
  if (!archivePipelineHeld_) {
    return;
  }
  archivePipelineHeld_ = false;
  archivePipelinePriorStatus_.reset();
  if (!dependencies_.endArchivePipeline) {
    return;
  }
  try {
    dependencies_.endArchivePipeline();
  } catch (...) {
    // Teardown and cancellation must remain non-throwing. The production
    // adapter only performs an atomic store here.
  }
}

ProfileResult ProfileSettingsController::create(std::string name) {
  if (!actionsEnabled() || !dependencies_.create) {
    auto result = unavailableResult("Profile creation is unavailable.");
    setFailure(result.error, result.message, {});
    return result;
  }
  try {
    return finishMutation(dependencies_.create(std::move(name)),
                          "Profile created.", std::nullopt);
  } catch (const std::exception &error) {
    auto result =
        profileFailure(ProfileError::IoFailure,
                       exceptionMessage(error, "Unable to create profile"));
    if (refreshAfterMutation(std::nullopt, result.message)) {
      setFailure(result.error, result.message, {});
    }
    return result;
  } catch (...) {
    auto result =
        profileFailure(ProfileError::IoFailure, "Unable to create profile.");
    if (refreshAfterMutation(std::nullopt, result.message)) {
      setFailure(result.error, result.message, {});
    }
    return result;
  }
}

ProfileResult ProfileSettingsController::rename(std::string_view profileId,
                                                std::string name) {
  if (!actionsEnabled() || !dependencies_.rename) {
    auto result = unavailableResult("Profile renaming is unavailable.");
    setFailure(result.error, result.message, {});
    return result;
  }
  if (!contains(profileId)) {
    auto result = profileFailure(ProfileError::NotFound,
                                 "The selected profile is unavailable.");
    setFailure(result.error, result.message, {});
    return result;
  }
  try {
    return finishMutation(dependencies_.rename(profileId, std::move(name)),
                          "Profile renamed.", std::string(profileId));
  } catch (const std::exception &error) {
    auto result =
        profileFailure(ProfileError::IoFailure,
                       exceptionMessage(error, "Unable to rename profile"));
    if (refreshAfterMutation(std::nullopt, result.message)) {
      setFailure(result.error, result.message, {});
    }
    return result;
  } catch (...) {
    auto result =
        profileFailure(ProfileError::IoFailure, "Unable to rename profile.");
    if (refreshAfterMutation(std::nullopt, result.message)) {
      setFailure(result.error, result.message, {});
    }
    return result;
  }
}

ProfileResult ProfileSettingsController::duplicate(std::string_view profileId,
                                                   std::string name) {
  if (!actionsEnabled() || !dependencies_.duplicate) {
    auto result = unavailableResult("Profile duplication is unavailable.");
    setFailure(result.error, result.message, {});
    return result;
  }
  if (!contains(profileId)) {
    auto result = profileFailure(ProfileError::NotFound,
                                 "The selected profile is unavailable.");
    setFailure(result.error, result.message, {});
    return result;
  }
  if (profileId == activeProfileId_) {
    std::string errorMessage;
    if (!flushActiveState(errorMessage)) {
      auto result =
          profileFailure(ProfileError::IoFailure, std::move(errorMessage));
      setFailure(result.error, result.message, {});
      return result;
    }
  }
  try {
    return finishMutation(dependencies_.duplicate(profileId, std::move(name)),
                          "Profile duplicated.", std::nullopt);
  } catch (const std::exception &error) {
    auto result =
        profileFailure(ProfileError::IoFailure,
                       exceptionMessage(error, "Unable to duplicate profile"));
    if (refreshAfterMutation(std::nullopt, result.message)) {
      setFailure(result.error, result.message, {});
    }
    return result;
  } catch (...) {
    auto result =
        profileFailure(ProfileError::IoFailure, "Unable to duplicate profile.");
    if (refreshAfterMutation(std::nullopt, result.message)) {
      setFailure(result.error, result.message, {});
    }
    return result;
  }
}

ProfileResult ProfileSettingsController::remove(std::string_view profileId) {
  const ProfileActionEligibility eligibility = deleteEligibility(profileId);
  if (!eligibility.enabled) {
    auto result =
        profileFailure(contains(profileId) ? ProfileError::SwitchBlocked
                                           : ProfileError::NotFound,
                       eligibility.reason);
    setFailure(result.error, result.message, {});
    return result;
  }
  if (!dependencies_.remove) {
    auto result = unavailableResult("Profile deletion is unavailable.");
    setFailure(result.error, result.message, {});
    return result;
  }
  try {
    return finishMutation(dependencies_.remove(profileId), "Profile deleted.",
                          std::nullopt);
  } catch (const std::exception &error) {
    auto result =
        profileFailure(ProfileError::IoFailure,
                       exceptionMessage(error, "Unable to delete profile"));
    if (refreshAfterMutation(std::nullopt, result.message)) {
      setFailure(result.error, result.message, {});
    }
    return result;
  } catch (...) {
    auto result =
        profileFailure(ProfileError::IoFailure, "Unable to delete profile.");
    if (refreshAfterMutation(std::nullopt, result.message)) {
      setFailure(result.error, result.message, {});
    }
    return result;
  }
}

ProfileSwitchResult
ProfileSettingsController::activate(std::string_view profileId) {
  if (!actionsEnabled() || !dependencies_.activate) {
    auto result = switchFailure(ProfileError::SwitchBlocked,
                                "Profile activation is unavailable.");
    setFailure(result.error, result.message, {});
    return result;
  }
  if (!contains(profileId)) {
    auto result = switchFailure(ProfileError::NotFound,
                                "The selected profile is unavailable.");
    setFailure(result.error, result.message, {});
    return result;
  }
  const std::string targetProfileId(profileId);
  const std::string activeProfileIdBefore = activeProfileId_;
  ProfileSwitchResult result;
  try {
    result = dependencies_.activate(targetProfileId);
  } catch (const std::exception &error) {
    result =
        switchFailure(ProfileError::IoFailure,
                      exceptionMessage(error, "Unable to activate profile"));
  } catch (...) {
    result =
        switchFailure(ProfileError::IoFailure, "Unable to activate profile.");
  }
  const bool refreshed = refreshAfterMutation(
      result.ok() ? std::optional<std::string>(targetProfileId) : std::nullopt,
      result.ok() ? std::string{} : result.message);
  const bool authoritativeCommit =
      refreshed && activeProfileId_ == targetProfileId &&
      (result.ok() || activeProfileIdBefore != targetProfileId);
  if (authoritativeCommit) {
    if (!result.ok()) {
      const std::string detail =
          result.message.empty()
              ? "the switch reported a follow-up error."
              : "the switch reported a follow-up error: " + result.message;
      result = {.message = "Profile activated, but " + detail};
    }
    setSuccess(result.message, "Profile activated.");
    return result;
  }
  if (!result.ok()) {
    if (refreshed) {
      setFailure(result.error, result.message, "Profile activation failed.");
    }
    return result;
  }
  if (refreshed) {
    result = switchFailure(
        ProfileError::IntegrityFailure,
        "Profile activation did not make the selected profile active.");
    setFailure(result.error, result.message, {});
  }
  return result;
}

ProfileResult
ProfileSettingsController::requestDelete(std::string_view profileId) {
  const auto eligibility = deleteEligibility(profileId);
  if (!eligibility.enabled) {
    auto result =
        profileFailure(contains(profileId) ? ProfileError::SwitchBlocked
                                           : ProfileError::NotFound,
                       eligibility.reason);
    setFailure(result.error, result.message, {});
    return result;
  }
  selectedProfileId_ = std::string(profileId);
  confirmationPriorStatus_ = status_;
  confirmationProfileId_ = selectedProfileId_;
  phase_ = ProfileSettingsPhase::ConfirmDelete;
  status_ = {.kind = ProfileSettingsStatusKind::Info,
             .message = "Choose Delete again to permanently remove this "
                        "profile."};
  const auto found =
      std::ranges::find_if(profiles_, [&](const PlayerProfile &candidate) {
        return candidate.id == profileId;
      });
  return {.profile = *found};
}

ProfileResult ProfileSettingsController::confirmDelete() {
  if (phase_ != ProfileSettingsPhase::ConfirmDelete ||
      confirmationProfileId_.empty()) {
    auto result = unavailableResult("There is no profile deletion to confirm.");
    setFailure(result.error, result.message, {});
    return result;
  }
  const std::string profileId = confirmationProfileId_;
  clearTransientPhase();
  return remove(profileId);
}

ProfileResult
ProfileSettingsController::requestOverwrite(std::string_view profileId) {
  const auto eligibility = overwriteEligibility(profileId);
  if (!eligibility.enabled) {
    auto result =
        profileFailure(contains(profileId) ? ProfileError::SwitchBlocked
                                           : ProfileError::NotFound,
                       eligibility.reason);
    setFailure(result.error, result.message, {});
    return result;
  }
  selectedProfileId_ = std::string(profileId);
  confirmationPriorStatus_ = status_;
  confirmationProfileId_ = selectedProfileId_;
  phase_ = ProfileSettingsPhase::ConfirmOverwrite;
  status_ = {.kind = ProfileSettingsStatusKind::Info,
             .message = "Choose Overwrite again to replace this profile from "
                        "the selected archive."};
  const auto found =
      std::ranges::find_if(profiles_, [&](const PlayerProfile &candidate) {
        return candidate.id == profileId;
      });
  return {.profile = *found};
}

void ProfileSettingsController::clearTransientPhase() {
  phase_ = ProfileSettingsPhase::Idle;
  confirmationProfileId_.clear();
  confirmationPriorStatus_.reset();
}

void ProfileSettingsController::cancelConfirmation() {
  if (phase_ == ProfileSettingsPhase::ConfirmDelete ||
      phase_ == ProfileSettingsPhase::ConfirmOverwrite) {
    const auto priorStatus = confirmationPriorStatus_;
    clearTransientPhase();
    status_ = priorStatus.value_or(ProfileSettingsStatus{});
  }
}

bool ProfileSettingsController::beginImportPicker() {
  if (!actionsEnabled() || !acquireArchivePipeline()) {
    return false;
  }
  phase_ = ProfileSettingsPhase::PickingImport;
  return true;
}

bool ProfileSettingsController::beginConfirmedOverwritePicker() {
  if (phase_ != ProfileSettingsPhase::ConfirmOverwrite ||
      confirmationProfileId_.empty() || !contains(confirmationProfileId_) ||
      !acquireArchivePipeline()) {
    return false;
  }
  archivePipelinePriorStatus_ =
      confirmationPriorStatus_.value_or(ProfileSettingsStatus{});
  phase_ = ProfileSettingsPhase::PickingImport;
  return true;
}

bool ProfileSettingsController::beginPreparedExportPicker(
    std::uint64_t generation) {
  if (phase_ != ProfileSettingsPhase::PreparingExport || generation == 0 ||
      generation != activeArchiveGeneration_ || !archivePipelineHeld_) {
    return false;
  }
  phase_ = ProfileSettingsPhase::PickingExport;
  status_ = {.kind = ProfileSettingsStatusKind::Info,
             .message = "Choose where to save the prepared profile archive."};
  return true;
}

void ProfileSettingsController::cancelPicker() {
  if (phase_ == ProfileSettingsPhase::PickingImport ||
      phase_ == ProfileSettingsPhase::PickingExport) {
    const auto priorStatus = archivePipelinePriorStatus_;
    activeArchiveGeneration_ = 0;
    clearTransientPhase();
    releaseArchivePipeline();
    status_ = priorStatus.value_or(ProfileSettingsStatus{});
  }
}

bool ProfileSettingsController::failPicker(std::string message) {
  if (phase_ != ProfileSettingsPhase::PickingImport &&
      phase_ != ProfileSettingsPhase::PickingExport) {
    return false;
  }
  activeArchiveGeneration_ = 0;
  clearTransientPhase();
  releaseArchivePipeline();
  setFailure(ProfileError::IoFailure, std::move(message),
             "Unable to access the selected profile archive document.");
  return true;
}

ProfileArchiveResult ProfileArchiveTask::execute() {
  if (!operation_) {
    return archiveFailure(ProfileError::SwitchBlocked,
                          "This profile archive task has already run.");
  }
  auto operation = std::move(operation_);
  operation_ = {};
  return operation();
}

std::optional<ProfileArchiveTask> ProfileSettingsController::beginExport(
    std::string_view profileId, const std::filesystem::path &destination) {
  if (!actionsEnabled() || !contains(profileId)) {
    setFailure(ProfileError::SwitchBlocked, {},
               "Finish the current profile action first.");
    return std::nullopt;
  }
  if (!acquireArchivePipeline()) {
    return std::nullopt;
  }
  if (destination.empty()) {
    setFailure(ProfileError::IoFailure, {},
               "Choose a profile archive destination.");
    clearTransientPhase();
    releaseArchivePipeline();
    return std::nullopt;
  }
  if (profileId == activeProfileId_) {
    std::string errorMessage;
    if (!flushActiveState(errorMessage)) {
      clearTransientPhase();
      releaseArchivePipeline();
      setFailure(ProfileError::IoFailure, std::move(errorMessage), {});
      return std::nullopt;
    }
  }
  if (!dependencies_.exportProfile) {
    clearTransientPhase();
    releaseArchivePipeline();
    setFailure(ProfileError::SwitchBlocked, {},
               "Profile export is unavailable.");
    return std::nullopt;
  }

  selectedProfileId_ = std::string(profileId);
  confirmationProfileId_.clear();
  confirmationPriorStatus_.reset();
  phase_ = ProfileSettingsPhase::PreparingExport;
  status_ = {.kind = ProfileSettingsStatusKind::Info,
             .message = "Preparing profile archive..."};
  const std::uint64_t generation = nextArchiveGeneration_++;
  activeArchiveGeneration_ = generation;
  auto operation = dependencies_.exportProfile;
  const std::string stableId(profileId);
  return ProfileArchiveTask(
      ProfileArchiveTaskKind::Export, generation,
      [operation = std::move(operation), stableId, destination]() mutable {
        try {
          return operation(stableId, destination);
        } catch (const std::exception &error) {
          return archiveFailure(
              ProfileError::IoFailure,
              exceptionMessage(error, "Unable to export profile"));
        } catch (...) {
          return archiveFailure(ProfileError::IoFailure,
                                "Unable to export profile.");
        }
      });
}

std::optional<ProfileArchiveTask>
ProfileSettingsController::beginImport(const std::filesystem::path &archive,
                                       const ProfileImportOptions &options) {
  const bool createImport = options.mode == ProfileImportMode::CreateWithNewId;
  const bool allowedCreate =
      createImport && (phase_ == ProfileSettingsPhase::Idle ||
                       (phase_ == ProfileSettingsPhase::PickingImport &&
                        confirmationProfileId_.empty()));
  bool allowedOverwrite = false;
  if (!createImport && options.overwriteProfileId) {
    allowedOverwrite = (phase_ == ProfileSettingsPhase::ConfirmOverwrite ||
                        phase_ == ProfileSettingsPhase::PickingImport) &&
                       confirmationProfileId_ == *options.overwriteProfileId;
  }
  if (!allowedCreate && !allowedOverwrite) {
    if (phase_ == ProfileSettingsPhase::PickingImport) {
      clearTransientPhase();
      releaseArchivePipeline();
    }
    setFailure(ProfileError::SwitchBlocked, {},
               createImport ? "Finish the current profile action first."
                            : "Confirm the overwrite target first.");
    return std::nullopt;
  }
  if (!acquireArchivePipeline()) {
    return std::nullopt;
  }
  if (allowedOverwrite && confirmationPriorStatus_) {
    archivePipelinePriorStatus_ = *confirmationPriorStatus_;
  }
  if (archive.empty()) {
    clearTransientPhase();
    releaseArchivePipeline();
    setFailure(ProfileError::IoFailure, {}, "Choose a profile archive file.");
    return std::nullopt;
  }
  if (!createImport) {
    const std::string &target = *options.overwriteProfileId;
    const ProfileSettingsPhase savedPhase = phase_;
    phase_ = ProfileSettingsPhase::Idle;
    const ProfileActionEligibility eligibility = overwriteEligibility(target);
    phase_ = savedPhase;
    if (!eligibility.enabled) {
      clearTransientPhase();
      releaseArchivePipeline();
      setFailure(ProfileError::SwitchBlocked, eligibility.reason, {});
      return std::nullopt;
    }
  }
  if (!dependencies_.importProfile) {
    clearTransientPhase();
    releaseArchivePipeline();
    setFailure(ProfileError::SwitchBlocked, {},
               "Profile import is unavailable.");
    return std::nullopt;
  }

  confirmationProfileId_.clear();
  confirmationPriorStatus_.reset();
  phase_ = ProfileSettingsPhase::Importing;
  status_ = {.kind = ProfileSettingsStatusKind::Info,
             .message = "Importing profile archive..."};
  const std::uint64_t generation = nextArchiveGeneration_++;
  activeArchiveGeneration_ = generation;
  auto operation = dependencies_.importProfile;
  return ProfileArchiveTask(
      ProfileArchiveTaskKind::Import, generation,
      [operation = std::move(operation), archive, options]() mutable {
        try {
          return operation(archive, options);
        } catch (const std::exception &error) {
          return archiveFailure(
              ProfileError::IoFailure,
              exceptionMessage(error, "Unable to import profile"));
        } catch (...) {
          return archiveFailure(ProfileError::IoFailure,
                                "Unable to import profile.");
        }
      });
}

bool ProfileSettingsController::completeArchive(
    ProfileArchiveTaskKind kind, std::uint64_t generation,
    const ProfileArchiveResult &result) {
  const bool expectedPhase =
      (kind == ProfileArchiveTaskKind::Export &&
       (phase_ == ProfileSettingsPhase::PreparingExport ||
        phase_ == ProfileSettingsPhase::PickingExport)) ||
      (kind == ProfileArchiveTaskKind::Import &&
       phase_ == ProfileSettingsPhase::Importing);
  if (!expectedPhase || generation == 0 ||
      generation != activeArchiveGeneration_) {
    return false;
  }

  activeArchiveGeneration_ = 0;
  clearTransientPhase();
  releaseArchivePipeline();
  const std::string preferred =
      result.profile ? result.profile->id : selectedProfileId_;
  const std::string operationError =
      result.ok()
          ? std::string{}
          : (result.message.empty() ? "The profile archive action failed."
                                    : result.message);
  const bool refreshed = refreshAfterMutation(preferred, operationError);
  if (!result.ok()) {
    if (refreshed) {
      setFailure(result.error, result.message,
                 "The profile archive action failed.");
    }
    return true;
  }
  if (refreshed) {
    setSuccess(result.message, kind == ProfileArchiveTaskKind::Export
                                   ? "Profile exported."
                                   : "Profile imported.");
  }
  return true;
}

void ProfileSettingsController::abandonArchive(std::uint64_t generation) {
  if (generation != 0 && generation == activeArchiveGeneration_) {
    activeArchiveGeneration_ = 0;
    clearTransientPhase();
    releaseArchivePipeline();
  }
}

ProfileArchiveResult ProfileSettingsController::exportProfile(
    std::string_view profileId, const std::filesystem::path &destination) {
  auto task = beginExport(profileId, destination);
  if (!task) {
    return unavailableArchiveResult(status_.message.empty()
                                        ? "Profile export could not start."
                                        : status_.message);
  }
  const auto kind = task->kind();
  const auto generation = task->generation();
  const ProfileArchiveResult result = task->execute();
  completeArchive(kind, generation, result);
  return result;
}

ProfileArchiveResult
ProfileSettingsController::importProfile(const std::filesystem::path &archive,
                                         const ProfileImportOptions &options) {
  auto task = beginImport(archive, options);
  if (!task) {
    return unavailableArchiveResult(status_.message.empty()
                                        ? "Profile import could not start."
                                        : status_.message);
  }
  const auto kind = task->kind();
  const auto generation = task->generation();
  const ProfileArchiveResult result = task->execute();
  completeArchive(kind, generation, result);
  return result;
}

void ProfileSettingsController::recordWarning(std::string message) {
  if (message.empty()) {
    return;
  }
  if (status_.kind == ProfileSettingsStatusKind::Warning &&
      !status_.message.empty() && status_.message != message) {
    status_.message += " " + message;
    return;
  }
  status_ = {.kind = ProfileSettingsStatusKind::Warning,
             .message = std::move(message)};
}
