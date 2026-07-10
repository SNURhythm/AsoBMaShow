#include "ProfileSessionCoordinator.h"

#include "AppSettingsStore.h"
#include "ProfileDatabaseActivity.h"
#include "ReplayDBHelper.h"
#include "ScoreDBHelper.h"
#include "input/InputProfileStore.h"

#include <exception>
#include <utility>

namespace {
ProfileSwitchResult switchFailure(ProfileError error, std::string message) {
  return {.error = error, .message = std::move(message)};
}

ProfileSwitchResult switchSuccess() { return {}; }

void appendRollbackError(std::string &message, std::string_view operation,
                         const std::exception &error) {
  if (!message.empty()) {
    message += "; ";
  }
  message += std::string(operation) + ": " + error.what();
}
} // namespace

ProfileSessionCoordinator::ProfileSessionCoordinator(
    PlayerProfileManager &manager, ScoreDBHelper &score, ReplayDBHelper &replay,
    Blocker blocker, ApplyInput applyInput, RestoreInput restoreInput,
    RefreshCaches refreshCaches, ProfileSessionDependencies dependencies)
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
        [](ScoreDBHelper &helper, const std::filesystem::path &path,
           std::string &error) { return helper.BindDatabasePath(path, error); };
  }
  if (!dependencies_.bindReplay) {
    dependencies_.bindReplay =
        [](ReplayDBHelper &helper, const std::filesystem::path &path,
           std::string &error) { return helper.BindDatabasePath(path, error); };
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

  profile_database_activity::SwitchGuard databaseGuard;
  if (!databaseGuard.ownsLock()) {
    return switchFailure(ProfileError::SwitchBlocked,
                         "A score or replay database operation is active.");
  }

  if (manager_.activeProfile().id == profileId) {
    return switchSuccess();
  }

  ProfileResult target;
  try {
    target = manager_.validateProfile(profileId);
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
  const std::filesystem::path oldScorePath = score_.GetDatabasePath();
  const std::filesystem::path oldReplayPath = replay_.GetDatabasePath();
  const AppSettings oldSettings = currentSettings;
  bool inputApplyAttempted = false;

  auto rollback = [&](ProfileError error, std::string message) {
    score_.SetDatabasePath(oldScorePath);
    replay_.SetDatabasePath(oldReplayPath);
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
  return switchSuccess();
}
