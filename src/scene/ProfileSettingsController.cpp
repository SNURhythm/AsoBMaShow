#include "ProfileSettingsController.h"

#include "../RAII.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace {
template <typename Result> struct PresentedResult : Result {
  i18n::Text text;
  PresentedResult() = default;
  PresentedResult(Result value) : Result(std::move(value)), text(this->message) {}
  PresentedResult(ProfileError error, i18n::Text value)
      : Result{.error = error, .message = value.resolve()}, text(std::move(value)) {}
};

PresentedResult<ProfileResult> profileFailure(ProfileError error, i18n::Text message) {
  return {error, std::move(message)};
}

PresentedResult<ProfileArchiveResult> archiveFailure(ProfileError error, i18n::Text message) {
  return {error, std::move(message)};
}

PresentedResult<ProfileSwitchResult> switchFailure(ProfileError error, i18n::Text message) {
  return {error, std::move(message)};
}

i18n::Text exceptionMessage(const std::exception &error, const char *operation) {
  return i18n::message(operation, {{"detail", error.what()}});
}

} // namespace

ProfileSettingsController::ProfileSettingsController(
    ProfileSettingsControllerDependencies dependencies)
    : dependencies_(std::move(dependencies)) {
  refresh();
}

ProfileSettingsController::~ProfileSettingsController() {
  abandonArchiveSkinMutation();
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
  status_.message = status_.text.resolve();
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
               i18n::message("settings.profiles.controller.services_unavailable"));
    return false;
  }
  ProfileListResult result;
  try {
    result = dependencies_.listProfiles();
  } catch (const std::exception &error) {
    setFailure(ProfileError::IoFailure,
               exceptionMessage(error, "settings.profiles.controller.read_failed_detail"), {});
    return false;
  } catch (...) {
    setFailure(ProfileError::IoFailure, {}, i18n::message("settings.profiles.controller.read_failed"));
    return false;
  }
  if (!result.ok()) {
    setFailure(result.error, std::move(result.message),
               i18n::message("settings.profiles.controller.read_failed"));
    return false;
  }
  if (result.profiles.empty()) {
    setFailure(ProfileError::IntegrityFailure, {},
               i18n::message("settings.profiles.controller.no_valid_profiles"));
    return false;
  }
  const bool activeExists =
      std::ranges::any_of(result.profiles, [&](const PlayerProfile &profile) {
        return profile.id == result.activeProfileId;
      });
  if (!activeExists) {
    setFailure(ProfileError::IntegrityFailure, {},
               i18n::message("settings.profiles.controller.active_missing"));
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
    std::string_view profileId) const {
  if (!contains(profileId)) {
    return {.enabled = false, .reason = i18n::message("settings.profiles.controller.unavailable").resolve(), .text = i18n::message("settings.profiles.controller.unavailable")};
  }
  if (profileId == activeProfileId_) {
    return {.enabled = false,
            .reason = i18n::message("settings.profiles.controller.activate_another").resolve(), .text = i18n::message("settings.profiles.controller.activate_another")};
  }
  if (profiles_.size() <= 1) {
    return {.enabled = false, .reason = i18n::message("settings.profiles.controller.keep_one").resolve(), .text = i18n::message("settings.profiles.controller.keep_one")};
  }
  if (phase_ != ProfileSettingsPhase::Idle) {
    return {.enabled = false,
            .reason = i18n::message("settings.profiles.controller.finish_current").resolve(), .text = i18n::message("settings.profiles.controller.finish_current")};
  }
  return {.enabled = true};
}

ProfileActionEligibility
ProfileSettingsController::deleteEligibility(std::string_view profileId) const {
  return destructiveEligibility(profileId);
}

ProfileActionEligibility ProfileSettingsController::overwriteEligibility(
    std::string_view profileId) const {
  return destructiveEligibility(profileId);
}

ProfileArchiveResult
ProfileSettingsController::unavailableArchiveResult(std::string message) const {
  return archiveFailure(ProfileError::SwitchBlocked, std::move(message));
}

void ProfileSettingsController::setFailure(ProfileError, i18n::Text message,
                                           i18n::Text fallback) {
  status_ = {.kind = ProfileSettingsStatusKind::Error,
             .text =
                 message.empty() ? std::move(fallback) : std::move(message)};
}

void ProfileSettingsController::setSuccess(i18n::Text message,
                                           i18n::Text fallback) {
  if (message.empty()) {
    status_ = {.kind = ProfileSettingsStatusKind::Success,
               .text = std::move(fallback)};
  } else {
    status_ = {.kind = ProfileSettingsStatusKind::Warning,
               .text = std::move(message)};
  }
}

bool ProfileSettingsController::refreshAfterMutation(
    std::optional<std::string> preferredProfileId,
    const i18n::Text &operationError) {
  const std::string priorSelection = selectedProfileId_;
  if (!refresh()) {
    if (!operationError.empty()) {
      const i18n::Text refreshError = status_.text;
      status_ = {.kind = ProfileSettingsStatusKind::Error,
                 .text = refreshError.empty() ? operationError :
                     i18n::message("settings.profiles.controller.combined_errors",
                                   {{"operation", operationError}, {"refresh", refreshError}})};
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
    ProfileResult result, i18n::Text successText,
    std::optional<std::string> preferredProfileId, i18n::Text failureText) {
  if (!result.ok()) {
    const i18n::Text operationError = !failureText.empty() ? failureText :
        (result.message.empty() ? i18n::message("settings.profiles.controller.action_failed") : i18n::Text(result.message));
    if (refreshAfterMutation(std::nullopt, operationError)) {
      setFailure(result.error, operationError, {});
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

bool ProfileSettingsController::flushActiveState(i18n::Text &errorMessage) {
  errorMessage = {};
  std::string diagnostic;
  if (!dependencies_.flushSettings) {
    errorMessage = i18n::message("settings.profiles.controller.settings_unavailable");
    return false;
  }
  try {
    if (!dependencies_.flushSettings(diagnostic)) {
      errorMessage = diagnostic;
      if (errorMessage.empty()) {
        errorMessage = i18n::message("settings.profiles.controller.save_active_settings_failed");
      }
      return false;
    }
  } catch (const std::exception &error) {
    errorMessage = exceptionMessage(error, "settings.profiles.controller.save_settings_failed_detail");
    return false;
  } catch (...) {
    errorMessage = i18n::message("settings.profiles.controller.save_settings_failed");
    return false;
  }
  if (!dependencies_.flushInput) {
    errorMessage = i18n::message("settings.profiles.controller.input_unavailable");
    return false;
  }
  try {
    if (!dependencies_.flushInput(diagnostic)) {
      errorMessage = diagnostic;
      if (errorMessage.empty()) {
        errorMessage = i18n::message("settings.profiles.controller.save_active_input_failed");
      }
      return false;
    }
  } catch (const std::exception &error) {
    errorMessage = exceptionMessage(error, "settings.profiles.controller.save_input_failed_detail");
    return false;
  } catch (...) {
    errorMessage = i18n::message("settings.profiles.controller.save_input_failed");
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
                 i18n::message("settings.profiles.controller.archive_busy"));
      return false;
    }
  } catch (const std::exception &error) {
    setFailure(ProfileError::SwitchBlocked,
               exceptionMessage(error, "settings.profiles.controller.archive_start_failed_detail"), {});
    return false;
  } catch (...) {
    setFailure(ProfileError::SwitchBlocked, {},
               i18n::message("settings.profiles.controller.archive_start_failed"));
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

std::optional<std::uint64_t>
ProfileSettingsController::beginSkinProfileCatalogMutation(
    std::optional<std::string_view> existingTarget,
    i18n::Text &errorMessage) {
  errorMessage = {};
  std::string diagnostic;
  if (!dependencies_.beginSkinProfileCatalogMutation) {
    if (nextFallbackSkinMutationToken_ ==
        std::numeric_limits<std::uint64_t>::max()) {
      errorMessage = i18n::message("settings.profiles.controller.tokens_exhausted");
      return std::nullopt;
    }
    return ++nextFallbackSkinMutationToken_;
  }
  try {
    auto token = dependencies_.beginSkinProfileCatalogMutation(existingTarget,
                                                               diagnostic);
    errorMessage = diagnostic;
    if (!token || *token == 0) {
      if (errorMessage.empty()) {
        errorMessage = i18n::message("settings.profiles.controller.skin_busy");
      }
      return std::nullopt;
    }
    return token;
  } catch (const std::exception &error) {
    errorMessage =
        exceptionMessage(error, "settings.profiles.controller.skin_fence_failed_detail");
  } catch (...) {
    errorMessage = i18n::message("settings.profiles.controller.skin_fence_failed");
  }
  return std::nullopt;
}

void ProfileSettingsController::finishSkinProfileCatalogMutation(
    std::uint64_t token, bool succeeded, bool profileStillExists) noexcept {
  if (token == 0 || !dependencies_.finishSkinProfileCatalogMutation) {
    return;
  }
  try {
    dependencies_.finishSkinProfileCatalogMutation(
        token, succeeded, profileStillExists);
  } catch (...) {
    // The production adapter is main-thread-only and no-throw. Teardown must
    // not strand the controller if a test or platform adapter violates that
    // contract.
  }
}

void ProfileSettingsController::abandonArchiveSkinMutation() noexcept {
  if (activeArchiveSkinMutationToken_ == 0) {
    return;
  }
  const auto token = std::exchange(activeArchiveSkinMutationToken_, 0);
  activeArchiveSkinMutationTarget_.reset();
  finishSkinProfileCatalogMutation(token, false, true);
}

ProfileResult ProfileSettingsController::create(std::string name) {
  if (!actionsEnabled() || !dependencies_.create) {
    auto result = profileFailure(ProfileError::SwitchBlocked, i18n::message("settings.profiles.controller.create_unavailable"));
    setFailure(result.error, result.text, {});
    return result;
  }
  i18n::Text barrierError;
  const auto token =
      beginSkinProfileCatalogMutation(std::nullopt, barrierError);
  if (!token) {
    auto result = profileFailure(ProfileError::SwitchBlocked,
                                 std::move(barrierError));
    setFailure(result.error, result.text, {});
    return result;
  }
  bool mutationSucceeded = false;
  ScopeExit finishBarrier([&] {
    finishSkinProfileCatalogMutation(*token, mutationSucceeded, true);
  });
  const auto profileCountBefore = profiles_.size();
  PresentedResult<ProfileResult> result;
  try {
    result = dependencies_.create(std::move(name));
  } catch (const std::exception &error) {
    result = profileFailure(
        ProfileError::IoFailure,
        exceptionMessage(error, "settings.profiles.controller.create_failed_detail"));
  } catch (...) {
    result =
        profileFailure(ProfileError::IoFailure, i18n::message("settings.profiles.controller.create_failed"));
  }
  result = finishMutation(result, i18n::message("settings.profiles.controller.created"), std::nullopt, result.text);
  mutationSucceeded =
      result.ok() || profiles_.size() > profileCountBefore;
  return result;
}

ProfileResult ProfileSettingsController::rename(std::string_view profileId,
                                                std::string name) {
  if (!actionsEnabled() || !dependencies_.rename) {
    auto result = profileFailure(ProfileError::SwitchBlocked, i18n::message("settings.profiles.controller.rename_unavailable"));
    setFailure(result.error, result.text, {});
    return result;
  }
  if (!contains(profileId)) {
    auto result = profileFailure(ProfileError::NotFound,
                                 i18n::message("settings.profiles.controller.selected_unavailable"));
    setFailure(result.error, result.text, {});
    return result;
  }
  try {
    return finishMutation(dependencies_.rename(profileId, std::move(name)),
                          i18n::message("settings.profiles.controller.renamed"), std::string(profileId));
  } catch (const std::exception &error) {
    auto result =
        profileFailure(ProfileError::IoFailure,
                       exceptionMessage(error, "settings.profiles.controller.rename_failed_detail"));
    if (refreshAfterMutation(std::nullopt, result.text)) {
      setFailure(result.error, result.text, {});
    }
    return result;
  } catch (...) {
    auto result =
        profileFailure(ProfileError::IoFailure, i18n::message("settings.profiles.controller.rename_failed"));
    if (refreshAfterMutation(std::nullopt, result.text)) {
      setFailure(result.error, result.text, {});
    }
    return result;
  }
}

ProfileResult ProfileSettingsController::duplicate(std::string_view profileId,
                                                   std::string name) {
  if (!actionsEnabled() || !dependencies_.duplicate) {
    auto result = profileFailure(ProfileError::SwitchBlocked, i18n::message("settings.profiles.controller.duplicate_unavailable"));
    setFailure(result.error, result.text, {});
    return result;
  }
  if (!contains(profileId)) {
    auto result = profileFailure(ProfileError::NotFound,
                                 i18n::message("settings.profiles.controller.selected_unavailable"));
    setFailure(result.error, result.text, {});
    return result;
  }
  if (profileId == activeProfileId_) {
    i18n::Text errorMessage;
    if (!flushActiveState(errorMessage)) {
      auto result =
          profileFailure(ProfileError::IoFailure, std::move(errorMessage));
      setFailure(result.error, result.text, {});
      return result;
    }
  }
  i18n::Text barrierError;
  const auto token =
      beginSkinProfileCatalogMutation(std::nullopt, barrierError);
  if (!token) {
    auto result = profileFailure(ProfileError::SwitchBlocked,
                                 std::move(barrierError));
    setFailure(result.error, result.text, {});
    return result;
  }
  bool mutationSucceeded = false;
  ScopeExit finishBarrier([&] {
    finishSkinProfileCatalogMutation(*token, mutationSucceeded, true);
  });
  const auto profileCountBefore = profiles_.size();
  PresentedResult<ProfileResult> result;
  try {
    result = dependencies_.duplicate(profileId, std::move(name));
  } catch (const std::exception &error) {
    result = profileFailure(
        ProfileError::IoFailure,
        exceptionMessage(error, "settings.profiles.controller.duplicate_failed_detail"));
  } catch (...) {
    result = profileFailure(ProfileError::IoFailure,
                            i18n::message("settings.profiles.controller.duplicate_failed"));
  }
  result =
      finishMutation(result, i18n::message("settings.profiles.controller.duplicated"), std::nullopt, result.text);
  mutationSucceeded =
      result.ok() || profiles_.size() > profileCountBefore;
  return result;
}

ProfileResult ProfileSettingsController::remove(std::string_view profileId) {
  const ProfileActionEligibility eligibility = deleteEligibility(profileId);
  if (!eligibility.enabled) {
    auto result =
        profileFailure(contains(profileId) ? ProfileError::SwitchBlocked
                                           : ProfileError::NotFound,
                       eligibility.text);
    setFailure(result.error, result.text, {});
    return result;
  }
  if (!dependencies_.remove) {
    auto result = profileFailure(ProfileError::SwitchBlocked, i18n::message("settings.profiles.controller.delete_unavailable"));
    setFailure(result.error, result.text, {});
    return result;
  }
  i18n::Text barrierError;
  const auto token = beginSkinProfileCatalogMutation(profileId, barrierError);
  if (!token) {
    auto result = profileFailure(ProfileError::SwitchBlocked,
                                 std::move(barrierError));
    setFailure(result.error, result.text, {});
    return result;
  }
  bool mutationSucceeded = false;
  bool profileStillExists = true;
  ScopeExit finishBarrier([&] {
    finishSkinProfileCatalogMutation(*token, mutationSucceeded,
                                     profileStillExists);
  });
  PresentedResult<ProfileResult> result;
  try {
    result = dependencies_.remove(profileId);
  } catch (const std::exception &error) {
    result = profileFailure(
        ProfileError::IoFailure,
        exceptionMessage(error, "settings.profiles.controller.delete_failed_detail"));
  } catch (...) {
    result =
        profileFailure(ProfileError::IoFailure, i18n::message("settings.profiles.controller.delete_failed"));
  }
  result = finishMutation(result, i18n::message("settings.profiles.controller.deleted"), std::nullopt, result.text);
  profileStillExists = contains(profileId);
  mutationSucceeded = result.ok() || !profileStillExists;
  return result;
}

ProfileSwitchResult
ProfileSettingsController::activate(std::string_view profileId) {
  if (!actionsEnabled() || !dependencies_.activate) {
    auto result = switchFailure(ProfileError::SwitchBlocked,
                                i18n::message("settings.profiles.controller.activate_unavailable"));
    setFailure(result.error, result.text, {});
    return result;
  }
  if (!contains(profileId)) {
    auto result = switchFailure(ProfileError::NotFound,
                                i18n::message("settings.profiles.controller.selected_unavailable"));
    setFailure(result.error, result.text, {});
    return result;
  }
  const std::string targetProfileId(profileId);
  const std::string activeProfileIdBefore = activeProfileId_;
  PresentedResult<ProfileSwitchResult> result;
  try {
    result = dependencies_.activate(targetProfileId);
  } catch (const std::exception &error) {
    result =
        switchFailure(ProfileError::IoFailure,
                      exceptionMessage(error, "settings.profiles.controller.activate_failed_detail"));
  } catch (...) {
    result =
        switchFailure(ProfileError::IoFailure, i18n::message("settings.profiles.controller.activate_failed"));
  }
  const bool refreshed = refreshAfterMutation(
      result.ok() ? std::optional<std::string>(targetProfileId) : std::nullopt,
      result.ok() ? i18n::Text{} : result.text);
  const bool authoritativeCommit =
      refreshed && activeProfileId_ == targetProfileId &&
      (result.ok() || activeProfileIdBefore != targetProfileId);
  if (authoritativeCommit) {
    if (!result.ok()) {
      result = PresentedResult<ProfileSwitchResult>(ProfileError::None,
          result.message.empty()
              ? i18n::message("settings.profiles.controller.activation_followup")
              : i18n::message("settings.profiles.controller.activation_followup_detail",
                              {{"detail", result.text}}));
    }
    setSuccess(result.text, i18n::message("settings.profiles.controller.activated"));
    return result;
  }
  if (!result.ok()) {
    if (refreshed) {
      setFailure(result.error, result.text, i18n::message("settings.profiles.controller.activation_failed"));
    }
    return result;
  }
  if (refreshed) {
    result = switchFailure(
        ProfileError::IntegrityFailure,
        i18n::message("settings.profiles.controller.activation_not_committed"));
    setFailure(result.error, result.text, {});
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
                       eligibility.text);
    setFailure(result.error, result.text, {});
    return result;
  }
  selectedProfileId_ = std::string(profileId);
  confirmationPriorStatus_ = status_;
  confirmationProfileId_ = selectedProfileId_;
  phase_ = ProfileSettingsPhase::ConfirmDelete;
  status_ = {.kind = ProfileSettingsStatusKind::Info, .text = {}};
  const auto found =
      std::ranges::find_if(profiles_, [&](const PlayerProfile &candidate) {
        return candidate.id == profileId;
      });
  return {.profile = *found};
}

ProfileResult ProfileSettingsController::confirmDelete() {
  if (phase_ != ProfileSettingsPhase::ConfirmDelete ||
      confirmationProfileId_.empty()) {
    auto result = profileFailure(ProfileError::SwitchBlocked, i18n::message("settings.profiles.controller.no_delete_confirmation"));
    setFailure(result.error, result.text, {});
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
                       eligibility.text);
    setFailure(result.error, result.text, {});
    return result;
  }
  selectedProfileId_ = std::string(profileId);
  confirmationPriorStatus_ = status_;
  confirmationProfileId_ = selectedProfileId_;
  phase_ = ProfileSettingsPhase::ConfirmOverwrite;
  status_ = {.kind = ProfileSettingsStatusKind::Info, .text = {}};
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
             .text = i18n::message("settings.profiles.controller.choose_save")};
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

bool ProfileSettingsController::failPicker(i18n::Text message) {
  if (phase_ != ProfileSettingsPhase::PickingImport &&
      phase_ != ProfileSettingsPhase::PickingExport) {
    return false;
  }
  activeArchiveGeneration_ = 0;
  clearTransientPhase();
  releaseArchivePipeline();
  setFailure(ProfileError::IoFailure, std::move(message),
             i18n::message("settings.profiles.controller.document_failed"));
  return true;
}

ProfileArchiveResult ProfileArchiveTask::execute() {
  if (!operation_) {
    return archiveFailure(ProfileError::SwitchBlocked,
                          i18n::message("settings.profiles.controller.task_already_run"));
  }
  auto operation = std::move(operation_);
  operation_ = {};
  return operation();
}

std::optional<ProfileArchiveTask> ProfileSettingsController::beginExport(
    std::string_view profileId, const std::filesystem::path &destination) {
  if (!actionsEnabled() || !contains(profileId)) {
    setFailure(ProfileError::SwitchBlocked, {},
               i18n::message("settings.profiles.controller.finish_current"));
    return std::nullopt;
  }
  if (!acquireArchivePipeline()) {
    return std::nullopt;
  }
  if (destination.empty()) {
    setFailure(ProfileError::IoFailure, {},
               i18n::message("settings.profiles.controller.choose_destination"));
    clearTransientPhase();
    releaseArchivePipeline();
    return std::nullopt;
  }
  if (profileId == activeProfileId_) {
    i18n::Text errorMessage;
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
               i18n::message("settings.profiles.controller.export_unavailable"));
    return std::nullopt;
  }

  selectedProfileId_ = std::string(profileId);
  confirmationProfileId_.clear();
  confirmationPriorStatus_.reset();
  phase_ = ProfileSettingsPhase::PreparingExport;
  status_ = {.kind = ProfileSettingsStatusKind::Info,
             .text = i18n::message("settings.profiles.controller.preparing")};
  const std::uint64_t generation = nextArchiveGeneration_++;
  activeArchiveGeneration_ = generation;
  auto operation = dependencies_.exportProfile;
  activeArchiveFailureText_ = std::make_shared<i18n::Text>();
  const auto failureText = activeArchiveFailureText_;
  const std::string stableId(profileId);
  return ProfileArchiveTask(
      ProfileArchiveTaskKind::Export, generation,
      [operation = std::move(operation), stableId, destination, failureText]() mutable -> ProfileArchiveResult {
        try {
          return operation(stableId, destination);
        } catch (const std::exception &error) {
          *failureText = exceptionMessage(error, "settings.profiles.controller.export_failed_detail");
          return archiveFailure(ProfileError::IoFailure, *failureText);
        } catch (...) {
          *failureText = i18n::message("settings.profiles.controller.export_failed");
          return archiveFailure(ProfileError::IoFailure, *failureText);
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
               createImport ? i18n::message("settings.profiles.controller.finish_current")
                            : i18n::message("settings.profiles.controller.confirm_overwrite"));
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
    setFailure(ProfileError::IoFailure, {}, i18n::message("settings.profiles.controller.choose_archive"));
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
      setFailure(ProfileError::SwitchBlocked, eligibility.text, {});
      return std::nullopt;
    }
  }
  if (!dependencies_.importProfile) {
    clearTransientPhase();
    releaseArchivePipeline();
    setFailure(ProfileError::SwitchBlocked, {},
               i18n::message("settings.profiles.controller.import_unavailable"));
    return std::nullopt;
  }

  i18n::Text barrierError;
  std::optional<std::string_view> existingTarget;
  if (!createImport) {
    existingTarget = *options.overwriteProfileId;
  }
  const auto skinMutationToken =
      beginSkinProfileCatalogMutation(existingTarget, barrierError);
  if (!skinMutationToken) {
    clearTransientPhase();
    releaseArchivePipeline();
    setFailure(ProfileError::SwitchBlocked, std::move(barrierError),
               i18n::message("settings.profiles.controller.skin_busy"));
    return std::nullopt;
  }

  activeArchiveSkinMutationToken_ = *skinMutationToken;
  try {
    confirmationProfileId_.clear();
    confirmationPriorStatus_.reset();
    phase_ = ProfileSettingsPhase::Importing;
    status_ = {.kind = ProfileSettingsStatusKind::Info,
               .text = i18n::message("settings.profiles.controller.importing")};
    const std::uint64_t generation = nextArchiveGeneration_++;
    activeArchiveGeneration_ = generation;
    activeArchiveSkinMutationTarget_ =
        createImport ? std::nullopt : options.overwriteProfileId;
    auto operation = dependencies_.importProfile;
    activeArchiveFailureText_ = std::make_shared<i18n::Text>();
    const auto failureText = activeArchiveFailureText_;
    return ProfileArchiveTask(
        ProfileArchiveTaskKind::Import, generation,
        [operation = std::move(operation), archive, options, failureText]() mutable -> ProfileArchiveResult {
          try {
            return operation(archive, options);
          } catch (const std::exception &error) {
            *failureText = exceptionMessage(error, "settings.profiles.controller.import_failed_detail");
            return archiveFailure(ProfileError::IoFailure, *failureText);
          } catch (...) {
            *failureText = i18n::message("settings.profiles.controller.import_failed");
            return archiveFailure(ProfileError::IoFailure, *failureText);
          }
        });
  } catch (...) {
    activeArchiveGeneration_ = 0;
    abandonArchiveSkinMutation();
    clearTransientPhase();
    releaseArchivePipeline();
    setFailure(ProfileError::IoFailure, {},
               i18n::message("settings.profiles.controller.retain_barrier_failed"));
    return std::nullopt;
  }
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

  const i18n::Text failureText = activeArchiveFailureText_
                                      ? *activeArchiveFailureText_ : i18n::Text{};
  activeArchiveFailureText_.reset();
  activeArchiveGeneration_ = 0;
  const auto skinMutationToken =
      std::exchange(activeArchiveSkinMutationToken_, 0);
  auto skinMutationTarget = std::move(activeArchiveSkinMutationTarget_);
  activeArchiveSkinMutationTarget_.reset();
  clearTransientPhase();
  releaseArchivePipeline();
  const std::string preferred =
      result.profile ? result.profile->id : selectedProfileId_;
  const i18n::Text operationError =
      result.ok()
          ? std::string{}
          : (!failureText.empty() ? failureText : result.message.empty() ? i18n::message("settings.profiles.controller.archive_failed")
                                    : result.message);
  const bool refreshed = refreshAfterMutation(preferred, operationError);
  const bool profileStillExists =
      !skinMutationTarget || contains(*skinMutationTarget);
  finishSkinProfileCatalogMutation(skinMutationToken, result.ok(),
                                   profileStillExists);
  if (!result.ok()) {
    if (refreshed) {
      setFailure(result.error, operationError,
                 i18n::message("settings.profiles.controller.archive_failed"));
    }
    return true;
  }
  if (refreshed) {
    setSuccess(result.message, kind == ProfileArchiveTaskKind::Export
                                   ? i18n::message("settings.profiles.controller.exported")
                                   : i18n::message("settings.profiles.controller.imported"));
  }
  return true;
}

void ProfileSettingsController::abandonArchive(std::uint64_t generation) {
  if (generation != 0 && generation == activeArchiveGeneration_) {
    activeArchiveGeneration_ = 0;
    abandonArchiveSkinMutation();
    clearTransientPhase();
    releaseArchivePipeline();
  }
}

ProfileArchiveResult ProfileSettingsController::exportProfile(
    std::string_view profileId, const std::filesystem::path &destination) {
  auto task = beginExport(profileId, destination);
  if (!task) {
    return unavailableArchiveResult(status_.text.empty()
                                        ? i18n::message("settings.profiles.controller.export_start_failed").resolve()
                                        : status_.text.resolve());
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
    return unavailableArchiveResult(status_.text.empty()
                                        ? i18n::message("settings.profiles.controller.import_start_failed").resolve()
                                        : status_.text.resolve());
  }
  const auto kind = task->kind();
  const auto generation = task->generation();
  const ProfileArchiveResult result = task->execute();
  completeArchive(kind, generation, result);
  return result;
}

void ProfileSettingsController::recordError(i18n::Text message) {
  if (message.empty()) {
    message = i18n::message("settings.profiles.controller.action_failed");
  }
  status_ = {.kind = ProfileSettingsStatusKind::Error,
             .text = std::move(message)};
}

void ProfileSettingsController::recordWarning(i18n::Text message) {
  if (message.empty()) {
    return;
  }
  if (status_.kind == ProfileSettingsStatusKind::Warning &&
      !status_.text.empty() && status_.text != message) {
    status_.text = i18n::message("settings.profiles.controller.combined_warnings",
                                  {{"prior", status_.text}, {"warning", message}});
    return;
  }
  status_ = {.kind = ProfileSettingsStatusKind::Warning,
             .text = std::move(message)};
}
