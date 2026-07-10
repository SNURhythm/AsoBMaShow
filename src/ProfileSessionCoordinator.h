#pragma once

#include "AppSettings.h"
#include "PlayerProfileManager.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

class ReplayDBHelper;
class ScoreDBHelper;

struct ProfileSwitchResult {
  ProfileError error = ProfileError::None;
  std::string message;

  [[nodiscard]] bool ok() const { return error == ProfileError::None; }
};

struct ProfileSessionDependencies {
  std::function<bool(const std::filesystem::path &, const AppSettings &,
                     std::string &)>
      saveSettings;
  std::function<bool(const std::filesystem::path &, std::string &)> saveInput;
  std::function<bool(ScoreDBHelper &, const std::filesystem::path &,
                     std::string &)>
      bindScore;
  std::function<bool(ReplayDBHelper &, const std::filesystem::path &,
                     std::string &)>
      bindReplay;
};

class ProfileSessionCoordinator {
public:
  using Blocker = std::function<std::optional<std::string>()>;
  using ApplyInput =
      std::function<bool(const std::filesystem::path &, std::string &)>;
  using RestoreInput = std::function<void(const std::filesystem::path &)>;
  using RefreshCaches = std::function<void()>;

  ProfileSessionCoordinator(PlayerProfileManager &manager, ScoreDBHelper &score,
                            ReplayDBHelper &replay, Blocker blocker,
                            ApplyInput applyInput, RestoreInput restoreInput,
                            RefreshCaches refreshCaches,
                            ProfileSessionDependencies dependencies = {});

  ProfileSwitchResult switchTo(std::string_view profileId,
                               AppSettings &currentSettings);

private:
  PlayerProfileManager &manager_;
  ScoreDBHelper &score_;
  ReplayDBHelper &replay_;
  Blocker blocker_;
  ApplyInput applyInput_;
  RestoreInput restoreInput_;
  RefreshCaches refreshCaches_;
  ProfileSessionDependencies dependencies_;
};
