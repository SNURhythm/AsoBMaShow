#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <mutex>
#include <iostream>
#include <memory>
#include <vector>
#include <string>
#include <SDL2/SDL.h>
#include "AppSettings.h"
#include "game/GameState.h"
#include "scene/SceneManager.h"
#include "audio/Jukebox.h"
#include "audio/AudioDeviceManager.h"
#include "audio/NativeMusicPlayer.h"
#include "audio/MusicPlayerService.h"
#include "input/InputDeviceRegistry.h"
#include "input/InputProfile.h"
#include "video/DisplaySettingsManager.h"
#include "video/FramePacer.h"
#include "video/RendererAccessCoordinator.h"
#include "view/UiTheme.h"
class ApplicationContext {

public:
  std::atomic<bool> quitFlag;
  SceneManager *sceneManager = nullptr;
  Uint64 currentFrame = 0;
  AppSettings settings;
  InputProfile inputProfile;
  InputDeviceRegistry inputDeviceRegistry;
  // Before replacing inputProfile, the profile-session owner must cancel or
  // destroy any InputCaptureController that references it. This prevents a
  // candidate staged from the prior profile from being confirmed into the
  // newly active profile.
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
      : quitFlag(false), settings(AppSettings::load()),
        inputProfile(makeDefaultInputProfile()), jukebox(&gameStopwatch),
        audioDeviceManager(jukebox.audioRuntime(), jukebox,
                           settings.audioVideo.audio) {
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
    ui_theme::setActiveMode(settings.uiThemeMode == AppSettings::UiThemeMode::Light
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
    std::cout << "Main function is quitting..." << std::endl;
  }
};
