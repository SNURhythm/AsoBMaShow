#include "ProfileSessionCoordinator.h"

#include "AppSettingsStore.h"
#include "ProfileDatabaseActivity.h"
#include "repositories/ReplayRepository.h"
#include "repositories/ScoreRepository.h"
#include "input/InputProfileStore.h"

#include <exception>
#include <utility>

namespace {
ProfileSwitchResult switchFailure(ProfileError error, std::string message) {
  return {.error = error, .message = std::move(message)};
}

ProfileSwitchResult switchSuccess(std::string message = {}) {
  return {.message = std::move(message)};
}

void appendRollbackError(std::string &message, std::string_view operation,
                         const std::exception &error) {
  if (!message.empty()) {
    message += "; ";
  }
  message += std::string(operation) + ": " + error.what();
}

void appendRollbackError(std::string &message, std::string_view operation,
                         const std::string &error) {
  if (!message.empty()) {
    message += "; ";
  }
  message += std::string(operation) + ": " + error;
}
} // namespace

ProfileSessionCoordinator::ProfileSessionCoordinator(
    PlayerProfileManager &manager, ScoreRepository &score,
    ReplayRepository &replay, Blocker blocker, ApplyInput applyInput,
    RestoreInput restoreInput, RefreshCaches refreshCaches,
    ProfileSessionDependencies dependencies)
    : manager_(manager), score_(score), replay_(replay),
      blocker_(std::move(blocker)), applyInput_(std::move(applyInput)),
      restoreInput_(std::move(restoreInput)),
      refreshCaches_(std::move(refreshCaches)),
      dependencies_(std::move(dependencies)) {
  if (!dependencies_.saveSettings) {
    dependencies_.saveSettings = AppSettingsStore::Save;
  }
  if (!dependencies_.saveInput) {
    dependencies_.saveInput = [](const std::filesystem::path &,
                                 std::string &error) {
      error = "active input profile persistence is not configured";
      return false;
    };
  }
  if (!dependencies_.bindScore) {
    dependencies_.bindScore =
        [](ScoreRepository &helper, const std::filesystem::path &path,
           std::string &error) { return helper.BindDatabasePath(path, error); };
  }
  if (!dependencies_.bindReplay) {
    dependencies_.bindReplay =
        [](ReplayRepository &helper, const std::filesystem::path &path,
           std::string &error) { return helper.BindDatabasePath(path, error); };
  }
  if (!dependencies_.recoverPendingResults) {
    dependencies_.recoverPendingResults = [] {
      return result_persistence::RecoverySummary{};
    };
  }
  if (!dependencies_.pauseProfileServices) {
    dependencies_.pauseProfileServices = [](std::string &) { return true; };
  }
  if (!dependencies_.activateProfileServices) {
    dependencies_.activateProfileServices = [](std::string_view,
                                               const AppSettings &,
                                               std::string &) { return true; };
  }
  if (!dependencies_.restoreProfileServices) {
    dependencies_.restoreProfileServices = [](std::string_view,
                                              const AppSettings &,
                                              std::string &) { return true; };
  }
}

ProfileSwitchResult
ProfileSessionCoordinator::switchTo(std::string_view profileId,
                                    AppSettings &currentSettings) {
  if (blocker_) {
    try {
      if (const auto reason = blocker_(); reason.has_value()) {
        return switchFailure(ProfileError::SwitchBlocked, *reason);
      }
    } catch (const std::exception &error) {
      return switchFailure(ProfileError::SwitchBlocked,
                           "Unable to evaluate profile switch blockers: " +
                               std::string(error.what()));
    }
  }

  const std::string oldProfileId = manager_.activeProfile().id;
  if (oldProfileId == profileId) {
    return switchSuccess();
  }

  ProfileResult target;
  try {
    target = manager_.validateProfileForActivation(profileId);
  } catch (const std::exception &error) {
    return switchFailure(ProfileError::IoFailure,
                         "Unable to validate target profile: " +
                             std::string(error.what()));
  }
  if (!target.ok()) {
    return switchFailure(target.error, target.message);
  }
  const PlayerProfilePaths targetPaths = manager_.pathsFor(profileId);
  const auto targetSettings = AppSettingsStore::Load(targetPaths.settingsJson);
  if (targetSettings.status == AppSettingsLoadStatus::FutureVersion) {
    return switchFailure(ProfileError::FutureVersion,
                         "Target profile settings are newer than supported.");
  }
  if (targetSettings.status != AppSettingsLoadStatus::Loaded) {
    return switchFailure(ProfileError::IntegrityFailure,
                         "Target profile settings could not be loaded.");
  }
  const auto targetInput = InputProfileStore::load(targetPaths.inputJson);
  if (targetInput.status == InputProfileLoadStatus::FutureVersion) {
    return switchFailure(ProfileError::FutureVersion,
                         "Target input profile is newer than supported.");
  }
  if (targetInput.status != InputProfileLoadStatus::Loaded) {
    return switchFailure(ProfileError::IntegrityFailure,
                         "Target input profile could not be loaded.");
  }

  const PlayerProfilePaths oldPaths = manager_.activePaths();
  const std::filesystem::path oldScorePath = oldPaths.scoresDb;
  const std::filesystem::path oldReplayPath = oldPaths.replaysDb;
  const AppSettings oldSettings = currentSettings;
  bool inputApplyAttempted = false;

  const auto restoreProfileServices = [&](std::string &message) {
    try {
      std::string restoreError;
      if (!dependencies_.restoreProfileServices(oldProfileId, oldSettings,
                                                restoreError)) {
        appendRollbackError(message, "unable to restore profile services",
                            restoreError);
      }
    } catch (const std::exception &error) {
      appendRollbackError(message, "unable to restore profile services", error);
    } catch (...) {
      appendRollbackError(message, "unable to restore profile services",
                          "unknown failure");
    }
  };

  std::string serviceError;
  try {
    if (!dependencies_.pauseProfileServices(serviceError)) {
      std::string message = "Unable to pause profile services: " + serviceError;
      restoreProfileServices(message);
      return switchFailure(ProfileError::SwitchBlocked, std::move(message));
    }
  } catch (const std::exception &error) {
    std::string message =
        "Unable to pause profile services: " + std::string(error.what());
    restoreProfileServices(message);
    return switchFailure(ProfileError::SwitchBlocked, std::move(message));
  } catch (...) {
    std::string message = "Unable to pause profile services: unknown failure";
    restoreProfileServices(message);
    return switchFailure(ProfileError::SwitchBlocked, std::move(message));
  }

  profile_database_activity::SwitchGuard databaseGuard;
  if (!databaseGuard.ownsLock()) {
    std::string message = "A score or replay database operation is active.";
    restoreProfileServices(message);
    return switchFailure(ProfileError::SwitchBlocked, std::move(message));
  }

  auto rollback = [&](ProfileError error, std::string message) {
    if (manager_.activeProfile().id != oldProfileId) {
      try {
        const ProfileResult restoredProfile =
            manager_.commitActiveProfile(oldProfileId);
        if (!restoredProfile.ok()) {
          appendRollbackError(message, "unable to restore active profile",
                              restoredProfile.message);
        }
      } catch (const std::exception &restoreError) {
        appendRollbackError(message, "unable to restore active profile",
                            restoreError);
      }
    }
    std::string databaseError;
    if (!score_.BindDatabasePath(oldScorePath, databaseError)) {
      appendRollbackError(message, "unable to restore score database",
                          databaseError);
      score_.SetDatabasePath(oldScorePath);
    }
    databaseError.clear();
    if (!replay_.BindDatabasePath(oldReplayPath, databaseError)) {
      appendRollbackError(message, "unable to restore replay database",
                          databaseError);
      replay_.SetDatabasePath(oldReplayPath);
    }
    currentSettings = oldSettings;
    if (inputApplyAttempted && restoreInput_) {
      try {
        if (dependencies_.beforeInputReplacement) {
          dependencies_.beforeInputReplacement();
        }
        restoreInput_(oldPaths.inputJson);
      } catch (const std::exception &restoreError) {
        appendRollbackError(message, "unable to restore input profile",
                            restoreError);
      }
    }
    if (refreshCaches_) {
      try {
        refreshCaches_();
      } catch (const std::exception &refreshError) {
        appendRollbackError(message, "unable to refresh restored caches",
                            refreshError);
      }
    }
    restoreProfileServices(message);
    return switchFailure(error, std::move(message));
  };

  std::string errorMessage;
  try {
    errorMessage.clear();
    if (!dependencies_.saveSettings(oldPaths.settingsJson, currentSettings,
                                    errorMessage)) {
      return rollback(ProfileError::IoFailure,
                      "Unable to save current profile settings: " +
                          errorMessage);
    }
  } catch (const std::exception &error) {
    return rollback(ProfileError::IoFailure,
                    "Unable to save current profile settings: " +
                        std::string(error.what()));
  }
  try {
    errorMessage.clear();
    if (!dependencies_.saveInput(oldPaths.inputJson, errorMessage)) {
      return rollback(ProfileError::IoFailure,
                      "Unable to save current input profile: " + errorMessage);
    }
  } catch (const std::exception &error) {
    return rollback(ProfileError::IoFailure,
                    "Unable to save current input profile: " +
                        std::string(error.what()));
  }
  try {
    errorMessage.clear();
    if (!dependencies_.bindScore(score_, targetPaths.scoresDb, errorMessage)) {
      return rollback(ProfileError::IoFailure,
                      "Unable to bind target score database: " + errorMessage);
    }
  } catch (const std::exception &error) {
    return rollback(ProfileError::IoFailure,
                    "Unable to bind target score database: " +
                        std::string(error.what()));
  }

  try {
    errorMessage.clear();
    if (!dependencies_.bindReplay(replay_, targetPaths.replaysDb,
                                  errorMessage)) {
      return rollback(ProfileError::IoFailure,
                      "Unable to bind target replay database: " + errorMessage);
    }
  } catch (const std::exception &error) {
    return rollback(ProfileError::IoFailure,
                    "Unable to bind target replay database: " +
                        std::string(error.what()));
  }

  std::string recoveryWarning;
  try {
    result_persistence::RecoverySummary recovery =
        dependencies_.recoverPendingResults();
    if (!recovery.userMessage.empty()) {
      recoveryWarning = std::move(recovery.userMessage);
    } else if (recovery.pending != 0 || recovery.conflicts != 0) {
      recoveryWarning = result_persistence::recoveryUserMessage();
    }
  } catch (const std::exception &) {
    recoveryWarning = result_persistence::recoveryUserMessage();
  } catch (...) {
    recoveryWarning = result_persistence::recoveryUserMessage();
  }

  inputApplyAttempted = true;
  try {
    if (dependencies_.beforeInputReplacement) {
      dependencies_.beforeInputReplacement();
    }
    errorMessage.clear();
    if (!applyInput_ || !applyInput_(targetPaths.inputJson, errorMessage)) {
      return rollback(ProfileError::IoFailure,
                      "Unable to apply target input profile: " + errorMessage);
    }
  } catch (const std::exception &error) {
    return rollback(ProfileError::IoFailure,
                    "Unable to apply target input profile: " +
                        std::string(error.what()));
  }
  currentSettings = targetSettings.settings;

  if (refreshCaches_) {
    try {
      refreshCaches_();
    } catch (const std::exception &error) {
      return rollback(ProfileError::IoFailure,
                      "Unable to refresh target profile caches: " +
                          std::string(error.what()));
    }
  }

  ProfileResult committed;
  try {
    committed = manager_.commitActiveProfile(profileId);
  } catch (const std::exception &error) {
    return rollback(ProfileError::IoFailure,
                    "Unable to commit active profile: " +
                        std::string(error.what()));
  }
  if (!committed.ok()) {
    return rollback(committed.error,
                    "Unable to commit active profile: " + committed.message);
  }

  try {
    errorMessage.clear();
    if (!dependencies_.activateProfileServices(profileId, currentSettings,
                                               errorMessage)) {
      return rollback(ProfileError::IoFailure,
                      "Unable to activate target profile services: " +
                          errorMessage);
    }
  } catch (const std::exception &error) {
    return rollback(ProfileError::IoFailure,
                    "Unable to activate target profile services: " +
                        std::string(error.what()));
  } catch (...) {
    return rollback(ProfileError::IoFailure,
                    "Unable to activate target profile services: unknown "
                    "failure");
  }
  return switchSuccess(std::move(recoveryWarning));
}
