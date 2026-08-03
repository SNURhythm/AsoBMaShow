#pragma once

#include "AppSettings.h"
#include "PlayerProfileManager.h"
#include "replay/ChartReplayPersistence.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

class ReplayRepository;
class ScoreRepository;

class NoThrowActiveProfileCommitted {
public:
  NoThrowActiveProfileCommitted() = default;

  template <typename Callback>
    requires(!std::is_same_v<std::remove_cvref_t<Callback>,
                             NoThrowActiveProfileCommitted>)
  NoThrowActiveProfileCommitted(Callback &&callback)
      : callback_(std::forward<Callback>(callback)) {}

  template <typename Callback>
    requires(!std::is_same_v<std::remove_cvref_t<Callback>,
                             NoThrowActiveProfileCommitted>)
  NoThrowActiveProfileCommitted &operator=(Callback &&callback) {
    callback_ = std::forward<Callback>(callback);
    return *this;
  }

  void operator()(std::string_view profileId,
                  AppSettings &settings) const noexcept {
    try {
      if (callback_) {
        callback_(profileId, settings);
      }
    } catch (...) {
    }
  }

  explicit operator bool() const noexcept {
    return static_cast<bool>(callback_);
  }

private:
  std::function<void(std::string_view, AppSettings &)> callback_;
};

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
  std::function<bool(std::string_view, AppSettings &, std::string &)>
      saveSettings;
  std::function<bool(const std::filesystem::path &, std::string &)> saveInput;
  std::function<bool(ScoreRepository &, const std::filesystem::path &,
                     std::string &)>
      bindScore;
  std::function<bool(ReplayRepository &, const std::filesystem::path &,
                     std::string &)>
      bindReplay;
  std::function<replay::ChartReplayRecoverySummary()> recoverPendingResults;
  std::function<void()> beforeInputReplacement;
  std::function<bool(std::string &)> pauseProfileServices;
  std::function<bool(std::string_view, const AppSettings &, std::string &)>
      activateProfileServices;
  std::function<bool(std::string_view, const AppSettings &, std::string &)>
      restoreProfileServices;
  NoThrowActiveProfileCommitted activeProfileCommitted;
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
