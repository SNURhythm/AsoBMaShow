#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <SDL2/SDL.h>
#include "AppSettings.h"
#include "AppSettingsStore.h"
#include "PlayerProfileManager.h"
#include "ProfileSessionCoordinator.h"
#include "ReplayDBHelper.h"
#include "ResultPersistenceCoordinator.h"
#include "ScoreDBHelper.h"
#include "Utils.h"
#include "game/GameState.h"
#include "scene/SceneManager.h"
#include "audio/Jukebox.h"
#include "audio/AudioDeviceManager.h"
#include "audio/NativeMusicPlayer.h"
#include "audio/MusicPlayerService.h"
#include "input/InputDeviceRegistry.h"
#include "input/InputProfile.h"
#include "input/InputProfileReplacementNotifier.h"
#include "input/InputProfileStore.h"
#include "video/DisplaySettingsManager.h"
#include "video/FramePacer.h"
#include "video/RendererAccessCoordinator.h"
#include "view/UiTheme.h"

namespace application_context_detail {
inline std::string firstDiagnostic(const std::vector<std::string> &diagnostics,
                                   std::string fallback) {
  return diagnostics.empty() ? std::move(fallback) : diagnostics.front();
}

inline AppSettings loadActiveSettings(PlayerProfileManager &manager,
                                      ProfileResult &initialization) {
  if (!initialization.ok()) {
    AppSettings defaults;
    defaults.sanitize();
    return defaults;
  }

  const AppSettingsLoadResult loaded =
      AppSettingsStore::Load(manager.activePaths().settingsJson);
  if (loaded.status == AppSettingsLoadStatus::Loaded) {
    return loaded.settings;
  }

  initialization.error = loaded.status == AppSettingsLoadStatus::FutureVersion
                             ? ProfileError::FutureVersion
                             : ProfileError::IntegrityFailure;
  initialization.message = firstDiagnostic(
      loaded.diagnostics, "Unable to load the active profile settings.");
  AppSettings defaults;
  defaults.sanitize();
  return defaults;
}

inline InputProfile loadActiveInput(PlayerProfileManager &manager,
                                    ProfileResult &initialization) {
  if (!initialization.ok()) {
    return makeDefaultInputProfile();
  }

  const InputProfileLoadResult loaded =
      InputProfileStore::load(manager.activePaths().inputJson);
  if (loaded.status == InputProfileLoadStatus::Loaded) {
    return loaded.profile;
  }

  initialization.error = loaded.status == InputProfileLoadStatus::FutureVersion
                             ? ProfileError::FutureVersion
                             : ProfileError::IntegrityFailure;
  initialization.message = firstDiagnostic(
      loaded.diagnostics, "Unable to load the active input profile.");
  return makeDefaultInputProfile();
}
} // namespace application_context_detail

class ApplicationContext {

public:
  std::atomic<bool> quitFlag;
  SceneManager *sceneManager = nullptr;
  Uint64 currentFrame = 0;
  std::filesystem::path applicationDataRoot;
  PlayerProfileManager profileManager;
  ProfileResult profileInitializationResult;
  AppSettings settings;
  InputProfile inputProfile;
  result_persistence::Coordinator resultPersistence;
  InputProfileReplacementNotifier inputProfileReplacementNotifier;
  std::unique_ptr<ProfileSessionCoordinator> profileSessionCoordinator;
  std::atomic<bool> profileGameplayActive{false};
  std::atomic<bool> profileArchiveOperationActive{false};
  std::atomic<std::uint64_t> profileCacheRevision{0};
  ProfileSwitchBlockers profileSwitchBlockers;
  std::function<void()> refreshProfileCaches;
  InputDeviceRegistry inputDeviceRegistry;
  // ProfileSessionCoordinator injects the active profile's input.json save
  // operation. Keeping the path owner outside the settings scene prevents
  // fallback to a machine-global legacy location.
  std::function<bool(const InputProfile &, std::string &)>
      saveActiveInputProfile = [](const InputProfile &, std::string &error) {
        error = "The active profile input path is not configured.";
        return false;
      };
  Jukebox jukebox;
  audio::AudioDeviceManager audioDeviceManager;
  audio::ApplyResult audioStartupApplyResult;
  music_player::MusicPlayerService musicPlayer;
  std::mutex bgfxRenderMutex;
  std::atomic<bool> replayVideoExportActive{false};
  display::RendererAccessCoordinator rendererAccess{bgfxRenderMutex,
                                                    replayVideoExportActive};
  std::atomic<bool> replayVideoExportUiFrameRequested{false};
  std::atomic<std::uint64_t> replayVideoExportUiFrameSerial{0};
  std::atomic<bool> appInBackground{false};
  std::atomic<bool> backgroundTasksPausedForForegroundScene{false};
  std::atomic<bool> ignoreBgaPostOptions{false};
  std::atomic<std::uint32_t> bgfxResetFlags{0};
  FramePacer framePacer;
  std::unique_ptr<display::IDisplayBackend> displayBackend;
  std::unique_ptr<display::DisplaySettingsManager> displaySettingsManager;
  std::function<void()> restoreGameplayRenderViews;
  std::function<void()> requestAddChartFolderFromFiles;
  std::function<void()> requestRebuildChartLibrary;
  std::function<void()> notifyBackgroundTaskPauseStateChanged;

  // string: annotation, thread: thread
  std::vector<std::pair<std::string, std::thread>> threads;

  ApplicationContext()
      : quitFlag(false), applicationDataRoot(Utils::GetDocumentsPath()),
        profileManager(applicationDataRoot),
        profileInitializationResult(profileManager.Initialize()),
        settings(application_context_detail::loadActiveSettings(
            profileManager, profileInitializationResult)),
        inputProfile(application_context_detail::loadActiveInput(
            profileManager, profileInitializationResult)),
        resultPersistence(ScoreDBHelper::GetInstance(),
                          ReplayDBHelper::GetInstance()),
        jukebox(&gameStopwatch),
        audioDeviceManager(jukebox.audioRuntime(), jukebox,
                           settings.audioVideo.audio) {
    if (!profileInitializationResult.ok()) {
      return;
    }

    inputDeviceRegistry.configureGyroscopeTurntable(
        inputProfile.gyroscopeTurntable);

    const PlayerProfilePaths activePaths = profileManager.activePaths();
    ScoreDBHelper::GetInstance().SetDatabasePath(activePaths.scoresDb);
    ReplayDBHelper::GetInstance().SetDatabasePath(activePaths.replaysDb);
    saveActiveInputProfile = [this](const InputProfile &candidate,
                                    std::string &error) {
      if (!profileInitializationResult.ok()) {
        error = profileInitializationResult.message.empty()
                    ? "Player profiles are not initialized."
                    : profileInitializationResult.message;
        return false;
      }
      return input_profile_runtime::saveThenApplyGyroscopeConfig(
          inputProfile, candidate,
          [this](const InputProfile &value, std::string &saveError) {
            return InputProfileStore::saveAtomic(
                profileManager.activePaths().inputJson, value, saveError);
          },
          [this](input::GyroscopeTurntableConfig config) {
            inputDeviceRegistry.configureGyroscopeTurntable(config);
          },
          error);
    };
    profileSessionCoordinator = std::make_unique<ProfileSessionCoordinator>(
        profileManager, ScoreDBHelper::GetInstance(),
        ReplayDBHelper::GetInstance(),
        [this]() -> std::optional<std::string> {
          if (profileGameplayActive.load(std::memory_order_acquire)) {
            return "A profile cannot be switched during gameplay.";
          }
          if (profileArchiveOperationActive.load(std::memory_order_acquire)) {
            return "A profile archive operation is active.";
          }
          if (replayVideoExportActive.load(std::memory_order_acquire)) {
            return "A replay export is active.";
          }
          return profileSwitchBlockers.firstReason();
        },
        [this](const std::filesystem::path &path, std::string &error) {
          const InputProfileLoadResult loaded = InputProfileStore::load(path);
          if (loaded.status != InputProfileLoadStatus::Loaded) {
            error = application_context_detail::firstDiagnostic(
                loaded.diagnostics, "Unable to load the target input profile.");
            return false;
          }
          inputProfile = loaded.profile;
          inputDeviceRegistry.configureGyroscopeTurntable(
              inputProfile.gyroscopeTurntable);
          return true;
        },
        [this](const std::filesystem::path &path) {
          const InputProfileLoadResult loaded = InputProfileStore::load(path);
          if (loaded.status != InputProfileLoadStatus::Loaded) {
            throw std::runtime_error(
                application_context_detail::firstDiagnostic(
                    loaded.diagnostics,
                    "Unable to restore the previous input profile."));
          }
          inputProfile = loaded.profile;
          inputDeviceRegistry.configureGyroscopeTurntable(
              inputProfile.gyroscopeTurntable);
        },
        [this]() {
          profileCacheRevision.fetch_add(1, std::memory_order_acq_rel);
          if (refreshProfileCaches) {
            refreshProfileCaches();
          }
        },
        ProfileSessionDependencies{
            .saveInput = [this](const std::filesystem::path &path,
                                std::string &error) {
              if (path != profileManager.activePaths().inputJson) {
                error = "The profile switch input path is not active.";
                return false;
              }
              return saveActiveInputProfile(inputProfile, error);
            },
            .beforeInputReplacement = [this]() {
              inputProfileReplacementNotifier.notifyBeforeReplacement();
            }});

    settings.sanitize();
    audioStartupApplyResult =
        audioDeviceManager.apply(settings.audioVideo.audio);
    if (audioStartupApplyResult.status != audio::ApplyStatus::Applied) {
      SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO,
                  "Saved audio settings were not fully applied: %s",
                  audioStartupApplyResult.message.empty()
                      ? "audio runtime remained on its effective working state"
                      : audioStartupApplyResult.message.c_str());
    }
    ui_theme::setActiveMode(settings.uiThemeMode ==
                                    AppSettings::UiThemeMode::Light
                                ? ui_theme::ThemeMode::Light
                                : ui_theme::ThemeMode::Dark);
    jukebox.setVisualsEnabled(settings.bgaEnabled);
    jukebox.setBgaOffsetMs(settings.audioOffsetMs);
    jukebox.setBgaDisplayMode(settings.bgaDisplayMode);
    std::string metadataVisibilityError;
    native_music_player::SetMetadataVisibility(
        {.showTitle = settings.systemPlaybackShowTitle,
         .showArtist = settings.systemPlaybackShowArtist,
         .showArtwork = settings.systemPlaybackShowJacket},
        metadataVisibilityError);
  }

  [[nodiscard]] bool profileReady() const {
    return profileInitializationResult.ok() &&
           profileSessionCoordinator != nullptr;
  }

  bool saveSettings(std::string *errorMessage = nullptr) {
    std::string error;
    if (!profileReady()) {
      error = profileInitializationResult.message.empty()
                  ? "Player profiles are not initialized."
                  : profileInitializationResult.message;
    } else if (AppSettingsStore::Save(profileManager.activePaths().settingsJson,
                                      settings, error)) {
      return true;
    }
    if (errorMessage != nullptr) {
      *errorMessage = std::move(error);
    }
    return false;
  }

  ProfileSwitchResult switchProfile(std::string_view profileId) {
    if (!profileSessionCoordinator) {
      return {.error = ProfileError::SwitchBlocked,
              .message = profileInitializationResult.message.empty()
                             ? "Player profiles are not initialized."
                             : profileInitializationResult.message};
    }
    return profileSessionCoordinator->switchTo(profileId, settings);
  }

  ~ApplicationContext() {
    quitFlag = true;
    std::string musicStopError;
    musicPlayer.Stop(musicStopError);
    std::cout << "Waiting for threads to join..." << std::endl;
    for (auto &thread : threads) {
      if (thread.second.joinable()) {
        std::cout << "Joining thread: " << thread.first << std::endl;
        thread.second.join();
      }
    }
    ScoreDBHelper::GetInstance().Shutdown();
    ReplayDBHelper::GetInstance().Shutdown();
    std::cout << "Main function is quitting..." << std::endl;
  }
};
