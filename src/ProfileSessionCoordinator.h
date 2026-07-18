#pragma once

#include "AppSettings.h"
#include "PlayerProfileManager.h"
#include "ResultPersistenceCoordinator.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

class ReplayRepository;
class ScoreRepository;

struct ProfileSwitchResult {
  ProfileError error = ProfileError::None;
  std::string message;

  [[nodiscard]] bool ok() const { return error == ProfileError::None; }
};

struct ProfileSwitchBlockers {
  using Blocker = std::function<std::optional<std::string>()>;

  Blocker background;
  Blocker scene;

  [[nodiscard]] std::optional<std::string> firstReason() const {
    if (background) {
      if (auto reason = background()) {
        return reason;
      }
    }
    return scene ? scene() : std::nullopt;
  }
};

struct ProfileSessionDependencies {
  std::function<bool(const std::filesystem::path &, const AppSettings &,
                     std::string &)>
      saveSettings;
  std::function<bool(const std::filesystem::path &, std::string &)> saveInput;
  std::function<bool(ScoreRepository &, const std::filesystem::path &,
                     std::string &)>
      bindScore;
  std::function<bool(ReplayRepository &, const std::filesystem::path &,
                     std::string &)>
      bindReplay;
  std::function<result_persistence::RecoverySummary()> recoverPendingResults;
  std::function<void()> beforeInputReplacement;
  std::function<bool(std::string &)> pauseProfileServices;
  std::function<bool(std::string_view, const AppSettings &, std::string &)>
      activateProfileServices;
  std::function<bool(std::string_view, const AppSettings &, std::string &)>
      restoreProfileServices;
};

class ProfileSessionCoordinator {
public:
  using Blocker = std::function<std::optional<std::string>()>;
  using ApplyInput =
      std::function<bool(const std::filesystem::path &, std::string &)>;
  using RestoreInput = std::function<void(const std::filesystem::path &)>;
  using RefreshCaches = std::function<void()>;

  ProfileSessionCoordinator(PlayerProfileManager &manager,
                            ScoreRepository &score, ReplayRepository &replay,
                            Blocker blocker, ApplyInput applyInput,
                            RestoreInput restoreInput,
                            RefreshCaches refreshCaches,
                            ProfileSessionDependencies dependencies = {});

  ProfileSwitchResult switchTo(std::string_view profileId,
                               AppSettings &currentSettings);

private:
  PlayerProfileManager &manager_;
  ScoreRepository &score_;
  ReplayRepository &replay_;
  Blocker blocker_;
  ApplyInput applyInput_;
  RestoreInput restoreInput_;
  RefreshCaches refreshCaches_;
  ProfileSessionDependencies dependencies_;
};
