#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <SDL2/SDL.h>
#include "AppSettings.h"
#include "AppSettingsStore.h"
#include "PlayerProfileManager.h"
#include "ProfileSessionCoordinator.h"
#include "repositories/ChartRepository.h"
#include "repositories/ReplayRepository.h"
#include "repositories/ScoreRepository.h"
#include "replay/ChartReplayPersistence.h"
#include "replay/CourseResultPersistence.h"
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
#include "ir/IrCredentialBackend.h"
#include "ir/IrCredentialMigration.h"
#include "ir/PendingIrCredentialCleanup.h"
#include "ir/IrHttpClient.h"
#include "ir/IrRankingService.h"
#include "ir/IrSubmissionService.h"
#include "ir/tachi/BokutachiCacheStore.h"
#include "ir/tachi/TachiDriver.h"
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
  ir::PendingIrCredentialCleanup pendingIrCredentialCleanup;
  PlayerProfileManager profileManager;
  ProfileResult profileInitializationResult;
  std::unique_ptr<ir::IrCredentialBackend> irCredentialBackend;
  AppSettings settings;
  InputProfile inputProfile;
  ChartRepository chartRepository;
  ScoreRepository scoreRepository;
  ReplayRepository replayRepository;
  MusicPlaylistRepository musicPlaylistRepository;
  std::shared_ptr<ir::tachi::BokutachiCacheStore> bokutachiCacheStore;
  ir::IrDriverRegistry irDrivers;
  std::unique_ptr<ir::IrHttpClient> irHttpClient;
  std::unique_ptr<ir::IrRankingService> irRankingService;
  std::unique_ptr<ir::IrSubmissionService> irSubmissionService;
  std::atomic<std::uint64_t> irAccountEvidenceRevision{0};
  std::mutex irCredentialMutex;
  std::set<std::string> irCredentialReadyProfiles;
  std::set<std::string> irCredentialBlockedProfiles;
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
        pendingIrCredentialCleanup(applicationDataRoot),
        profileManager(applicationDataRoot),
        profileInitializationResult(profileManager.Initialize()),
        irCredentialBackend(
            ir::CreatePlatformIrCredentialBackend(applicationDataRoot)),
        settings(application_context_detail::loadActiveSettings(
            profileManager, profileInitializationResult)),
        inputProfile(application_context_detail::loadActiveInput(
            profileManager, profileInitializationResult)),
        bokutachiCacheStore(std::make_shared<ir::tachi::BokutachiCacheStore>()),
        jukebox(&gameStopwatch),
        audioDeviceManager(jukebox.audioRuntime(), jukebox,
                           settings.audioVideo.audio),
        musicPlayer(musicPlaylistRepository, chartRepository) {
    std::string irDriverDiagnostic;
    if (!irDrivers.registerDriver(
            std::make_shared<ir::tachi::TachiDriver>(bokutachiCacheStore),
            irDriverDiagnostic)) {
      SDL_Log("Bokutachi IR driver registration was unavailable");
    }
    if (!profileInitializationResult.ok()) {
      return;
    }

    retryPendingIrCredentialCleanup();

    inputDeviceRegistry.configureGyroscopeTurntable(
        inputProfile.gyroscopeTurntable);

    const PlayerProfilePaths activePaths = profileManager.activePaths();
    scoreRepository.SetDatabasePath(activePaths.scoresDb);
    replayRepository.SetDatabasePath(activePaths.replaysDb);
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
        profileManager, scoreRepository, replayRepository,
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
            .saveInput =
                [this](const std::filesystem::path &path, std::string &error) {
                  if (path != profileManager.activePaths().inputJson) {
                    error = "The profile switch input path is not active.";
                    return false;
                  }
                  return saveActiveInputProfile(inputProfile, error);
                },
            .recoverPendingResults = [this] { return recoverPendingResults(); },
            .beforeInputReplacement =
                [this]() {
                  inputProfileReplacementNotifier.notifyBeforeReplacement();
                },
            .pauseProfileServices =
                [this](std::string &error) {
                  return pauseIrProfileServices(error);
                },
            .activateProfileServices =
                [this](std::string_view profileId,
                       const AppSettings &activeSettings, std::string &error) {
                  return activateIrProfileServices(profileId, activeSettings,
                                                   error);
                },
            .restoreProfileServices =
                [this](std::string_view profileId,
                       const AppSettings &activeSettings, std::string &error) {
                  return activateIrProfileServices(profileId, activeSettings,
                                                   error);
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

  [[nodiscard]] replay::ChartReplayRecoverySummary
  recoverPendingResults() noexcept {
    try {
      if (!profileInitializationResult.ok()) {
        return {};
      }
      replay::ChartReplayPersistence chartPersistence(scoreRepository,
                                                      replayRepository);
      auto summary = chartPersistence.recoverAll();
      replay::CourseResultPersistence coursePersistence(scoreRepository,
                                                        replayRepository);
      const auto courseRecovery = coursePersistence.recoverAll();
      summary.attempted += courseRecovery.attempted;
      summary.saved += courseRecovery.saved;
      summary.pending += courseRecovery.pending;
      summary.conflicts += courseRecovery.conflicts;
      if (!courseRecovery.diagnostic.empty()) {
        if (!summary.diagnostic.empty()) {
          summary.diagnostic.push_back('\n');
        }
        summary.diagnostic.append("course score recovery: ");
        summary.diagnostic.append(courseRecovery.diagnostic);
      }
      SDL_Log("Result recovery completed: attempted=%zu saved=%zu pending=%zu "
              "conflicts=%zu",
              summary.attempted, summary.saved, summary.pending,
              summary.conflicts);
      if (!summary.diagnostic.empty()) {
        SDL_Log("Result recovery reported technical diagnostics");
      }
      return summary;
    } catch (const std::exception &) {
      SDL_Log("Result recovery raised a standard exception");
      return {.pending = 1,
              .diagnostic =
                  "modern result recovery raised a standard exception"};
    } catch (...) {
      SDL_Log("Result recovery raised a non-standard exception");
      return {.pending = 1,
              .diagnostic =
                  "modern result recovery raised a non-standard exception"};
    }
  }

  [[nodiscard]] replay::ChartReplayPersistenceOutcome
  persistModernChart(const replay::ChartReplayPersistenceAttempt &attempt,
      std::span<const ir::IrOutboxDraft> drafts = {}) noexcept {
    try {
      if (!profileInitializationResult.ok()) {
        return {.state = replay::ChartReplayPersistenceState::Retryable,
                .diagnostic = "Player profiles are not initialized."};
      }
      replay::ChartReplayPersistence persistence(scoreRepository,
                                                 replayRepository);
      return persistence.persist(attempt, drafts);
    } catch (const std::exception &error) {
      return {.state = replay::ChartReplayPersistenceState::Retryable,
              .diagnostic = std::string("Modern chart persistence failed: ") +
                            error.what()};
    } catch (...) {
      return {.state = replay::ChartReplayPersistenceState::Retryable,
              .diagnostic = "Modern chart persistence failed."};
    }
  }

  [[nodiscard]] replay::CourseResultPersistenceOutcome persistModernCourse(
      const replay::CapturedCourseReplayAttempt &attempt) noexcept {
    try {
      if (!profileInitializationResult.ok()) {
        return {.state = replay::CourseResultPersistenceState::Retryable,
                .diagnostic = "Player profiles are not initialized."};
      }
      replay::CourseResultPersistence persistence(scoreRepository,
                                                  replayRepository);
      return persistence.persist(attempt);
    } catch (const std::exception &error) {
      return {.state = replay::CourseResultPersistenceState::Retryable,
              .diagnostic = std::string("Modern course persistence failed: ") +
                            error.what()};
    } catch (...) {
      return {.state = replay::CourseResultPersistenceState::Retryable,
              .diagnostic = "Modern course persistence failed."};
    }
  }

  [[nodiscard]] bool profileReady() const {
    return profileInitializationResult.ok() &&
           profileSessionCoordinator != nullptr;
  }

  [[nodiscard]] ir::IrActiveProfileConfig
  irProfileConfig(std::string_view profileId,
                  const AppSettings &profileSettings) const {
    ir::IrActiveProfileConfig config{.profileId = std::string(profileId)};
    for (const auto &[providerId, stored] : profileSettings.irProviders) {
      ir::IrProviderSettings sanitized = stored;
      ir::sanitizeProviderSettings(sanitized);
      config.providers.emplace(providerId, std::move(sanitized));
    }
    return config;
  }

  [[nodiscard]] std::string
  lookupActiveIrCredential(std::string_view profileId,
                           std::string_view providerId) {
    if (!profileInitializationResult.ok() ||
        profileManager.activeProfile().id != profileId) {
      return {};
    }
    std::optional<std::string> apiKey;
    std::string diagnostic;
    if (!loadIrCredential(profileId, providerId, apiKey, diagnostic) ||
        !apiKey) {
      return {};
    }
    return std::move(*apiKey);
  }

  bool prepareIrCredentials(std::string_view profileId,
                            std::string &diagnostic) noexcept {
    try {
      std::lock_guard lock(irCredentialMutex);
      const std::string identity(profileId);
      if (irCredentialReadyProfiles.contains(identity)) {
        return true;
      }
      if (irCredentialBlockedProfiles.contains(identity)) {
        diagnostic =
            "Secure IR credential migration is unavailable this session.";
        return false;
      }
      if (!irCredentialBackend) {
        irCredentialBlockedProfiles.insert(identity);
        diagnostic = "Secure IR credential storage is unavailable.";
        return false;
      }
      if (irCredentialBackend->requiresLegacyFileMigration()) {
        const auto migrated = ir::migrateLegacyIrCredentials(
            profileId, profileManager.pathsFor(profileId).irCredentialsJson,
            *irCredentialBackend);
        if (!migrated.ready()) {
          irCredentialBlockedProfiles.insert(identity);
          diagnostic = migrated.diagnostic.empty()
                           ? "Secure IR credential migration failed."
                           : migrated.diagnostic;
          return false;
        }
      }
      irCredentialReadyProfiles.insert(identity);
      return true;
    } catch (...) {
      irCredentialBlockedProfiles.insert(std::string(profileId));
      diagnostic = "Secure IR credential storage failed unexpectedly.";
      return false;
    }
  }

  bool loadIrCredential(std::string_view profileId,
                        std::string_view providerId,
                        std::optional<std::string> &apiKey,
                        std::string &diagnostic) noexcept {
    apiKey.reset();
    if (!prepareIrCredentials(profileId, diagnostic)) {
      return false;
    }
    std::lock_guard lock(irCredentialMutex);
    const auto loaded = irCredentialBackend->load(profileId, providerId);
    if (loaded.status == ir::IrCredentialBackendReadStatus::Missing) {
      return true;
    }
    if (loaded.status != ir::IrCredentialBackendReadStatus::Loaded ||
        !loaded.apiKey) {
      diagnostic = loaded.diagnostic.empty()
                       ? "Secure IR credential could not be read."
                       : loaded.diagnostic;
      return false;
    }
    apiKey = std::move(loaded.apiKey);
    return true;
  }

  bool replaceIrCredential(std::string_view profileId,
                           std::string_view providerId,
                           std::string_view apiKey,
                           std::string &diagnostic) noexcept {
    if (!prepareIrCredentials(profileId, diagnostic)) {
      return false;
    }
    std::lock_guard lock(irCredentialMutex);
    auto result = irCredentialBackend->replace(profileId, providerId, apiKey);
    diagnostic = std::move(result.diagnostic);
    return result.succeeded;
  }

  bool removeIrCredential(std::string_view profileId,
                          std::string_view providerId,
                          std::string &diagnostic) noexcept {
    if (!prepareIrCredentials(profileId, diagnostic)) {
      return false;
    }
    std::lock_guard lock(irCredentialMutex);
    auto result = irCredentialBackend->remove(profileId, providerId);
    diagnostic = std::move(result.diagnostic);
    return result.succeeded;
  }

  bool removeProfileIrCredentials(std::string_view profileId,
                                  std::string &diagnostic) noexcept {
    try {
      std::lock_guard lock(irCredentialMutex);
      if (!irCredentialBackend) {
        diagnostic = "Secure IR credential storage is unavailable.";
        return false;
      }
      auto result = irCredentialBackend->removeProfile(profileId);
      diagnostic = std::move(result.diagnostic);
      if (result.succeeded) {
        irCredentialReadyProfiles.erase(std::string(profileId));
        irCredentialBlockedProfiles.erase(std::string(profileId));
      }
      return result.succeeded;
    } catch (...) {
      diagnostic = "Profile IR credentials could not be removed.";
      return false;
    }
  }

  void retryPendingIrCredentialCleanup() noexcept {
    try {
      const auto overwriteRetried =
          ir::retryPendingProfileCredentialOverwriteCleanup(
              pendingIrCredentialCleanup,
              [this](std::string_view profileId, std::string &diagnostic) {
                return removeProfileIrCredentials(profileId, diagnostic);
              });
      if (overwriteRetried.retained != 0 ||
          !overwriteRetried.diagnostic.empty()) {
        SDL_Log("Pending overwritten-profile credential cleanup retained %zu "
                "item(s): %s",
                overwriteRetried.retained,
                overwriteRetried.diagnostic.c_str());
      }
      const auto retried = ir::retryPendingProfileCredentialCleanup(
          pendingIrCredentialCleanup,
          [this](std::string_view profileId) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(
                profileManager.pathsFor(profileId).root, error);
            if (error == std::errc::no_such_file_or_directory ||
                status.type() == std::filesystem::file_type::not_found) {
              return false;
            }
            // Treat inspection failures conservatively: a profile that might
            // still exist must keep its secure credentials.
            return error || status.type() !=
                                std::filesystem::file_type::not_found;
          },
          [this](std::string_view profileId, std::string &diagnostic) {
            return removeProfileIrCredentials(profileId, diagnostic);
          });
      if (retried.retained != 0 || !retried.diagnostic.empty()) {
        SDL_Log("Pending secure IR credential cleanup retained %zu item(s): %s",
                retried.retained, retried.diagnostic.c_str());
      }
    } catch (...) {
      SDL_Log("Pending secure IR credential cleanup could not be retried");
    }
  }

  bool projectMirroredIrScores(std::string_view profileId,
                               std::string_view providerId,
                               std::string_view serverOrigin,
                               std::string &diagnostic) noexcept {
    try {
      if (!profileReady() || profileManager.activeProfile().id != profileId) {
        diagnostic = "The active profile changed during IR score import.";
        return false;
      }
      const auto state = replayRepository.LoadIrRemoteScoreMirrorState(
          providerId, serverOrigin);
      if (state.status != ir::IrRemoteScoreMirrorStateOutcome::Status::Loaded) {
        diagnostic = state.diagnostic.empty()
                         ? "The synchronized IR score mirror is unavailable."
                         : state.diagnostic;
        return false;
      }
      if (scoreRepository.ImportedIrScoresAreCurrent(providerId, serverOrigin,
                                                     state.syncGeneration,
              state.scoreCount)) {
        return true;
      }
      const auto mirrored =
          replayRepository.ListIrRemoteScores(providerId, serverOrigin);
      if (mirrored.status != ir::IrRemoteScoreReadOutcome::Status::Loaded ||
          mirrored.scores.size() != state.scoreCount) {
        diagnostic = mirrored.diagnostic.empty()
                         ? "The synchronized IR score mirror is inconsistent."
                         : mirrored.diagnostic;
        return false;
      }
      const std::int64_t generation =
          state.syncGeneration > 0 ? state.syncGeneration : 1;
      const auto projected = scoreRepository.ReplaceImportedIrScores(
          providerId, serverOrigin, generation, mirrored.scores);
      if (projected.status == ImportedIrScoreProjectionStatus::Applied ||
          projected.status == ImportedIrScoreProjectionStatus::AlreadyCurrent) {
        return true;
      }
      diagnostic = projected.diagnostic.empty()
                       ? "Synchronized IR scores could not be imported."
                       : projected.diagnostic;
      return false;
    } catch (...) {
      diagnostic = "Synchronized IR scores could not be imported.";
      return false;
    }
  }

  void restoreMirroredIrScores(std::string_view profileId,
                               const AppSettings &profileSettings) noexcept {
    for (const auto &[providerId, stored] : profileSettings.irProviders) {
      ir::IrProviderSettings provider = stored;
      ir::sanitizeProviderSettings(provider);
      std::string diagnostic;
      if (!projectMirroredIrScores(profileId, providerId, provider.serverOrigin,
                                   diagnostic)) {
        SDL_Log("Synchronized IR score import could not be restored: %s",
                diagnostic.c_str());
      }
    }
  }

  bool startIrServices() noexcept {
    try {
      if (!profileReady()) {
        return false;
      }
      std::string credentialDiagnostic;
      if (!prepareIrCredentials(profileManager.activeProfile().id,
                                credentialDiagnostic)) {
        SDL_Log("Authenticated IR is disabled for the active profile");
      }
      if (!irHttpClient) {
        irHttpClient = ir::CreatePlatformIrHttpClient();
      }
      if (!irHttpClient) {
        SDL_Log("IR HTTP transport was unavailable");
        return false;
      }
      std::string cacheDiagnostic;
      if (!bokutachiCacheStore->activate(
              profileManager.activePaths().bokutachiCacheJson,
              cacheDiagnostic)) {
        SDL_Log(
            "Bokutachi lookup cache was unavailable; using network lookups");
      }
      if (!irRankingService) {
        ir::IrRankingServiceOptions options;
        options.credentialLookup = [this](std::string_view profileId,
                                          std::string_view providerId) {
          return lookupActiveIrCredential(profileId, providerId);
        };
        irRankingService = std::make_unique<ir::IrRankingService>(
            irDrivers, *irHttpClient, std::move(options));
      }
      if (!irSubmissionService) {
        ir::IrSubmissionServiceOptions options;
        options.credentialLookup = [this](std::string_view profileId,
                                          std::string_view providerId) {
          return lookupActiveIrCredential(profileId, providerId);
        };
        options.submissionSucceeded =
            [this](std::string_view profileId, std::string_view providerId,
                   std::string_view requestOrigin, std::string_view,
                   std::string_view chartSha256) {
              if (irRankingService) {
                irRankingService->invalidate({
                    .profileId = std::string(profileId),
                    .providerId = std::string(providerId),
                    .serverOrigin = std::string(requestOrigin),
                    .chartSha256 = std::string(chartSha256),
                    .clearVisible = false,
                });
              }
            };
        options.credentialChanged = [this](std::string_view profileId,
                                           std::string_view providerId) {
          if (providerId == "tachi") {
            std::string cacheDiagnostic;
            if (!bokutachiCacheStore->clearUserIds(cacheDiagnostic)) {
              SDL_Log("Bokutachi cached identity could not be invalidated");
            }
          }
          if (irRankingService) {
            irRankingService->invalidate({
                .profileId = std::string(profileId),
                .providerId = std::string(providerId),
            });
          }
        };
        options.remoteSnapshotApplied =
            [this](std::string_view profileId, std::string_view providerId,
                   std::string_view serverOrigin, std::int64_t syncGeneration,
                   std::span<const ir::IrRemoteScore> scores,
                   std::string &diagnostic) {
              if (!profileReady() ||
                  profileManager.activeProfile().id != profileId) {
                diagnostic =
                    "The active profile changed during IR score import.";
                return false;
              }
              const auto projected = scoreRepository.ReplaceImportedIrScores(
                  providerId, serverOrigin, syncGeneration, scores);
              if (projected.status ==
                      ImportedIrScoreProjectionStatus::Applied ||
                  projected.status ==
                      ImportedIrScoreProjectionStatus::AlreadyCurrent) {
                return true;
              }
              diagnostic = projected.diagnostic.empty()
                               ? "Synchronized IR scores could not be imported."
                               : projected.diagnostic;
              return false;
            };
        irSubmissionService = std::make_unique<ir::IrSubmissionService>(
            replayRepository, irDrivers, *irHttpClient, std::move(options));
      }
      const std::string profileId = profileManager.activeProfile().id;
      restoreMirroredIrScores(profileId, settings);
      irRankingService->activateProfile(profileId);
      irSubmissionService->start(irProfileConfig(profileId, settings));
      irSubmissionService->setApplicationActive(
          !appInBackground.load(std::memory_order_acquire));
      return true;
    } catch (...) {
      SDL_Log("IR services could not be started");
      return false;
    }
  }

  bool pauseIrProfileServices(std::string &error) noexcept {
    try {
      if (irSubmissionService) {
        irSubmissionService->pauseAndCancel();
      }
      if (irRankingService) {
        irRankingService->pauseAndCancel();
      }
      return true;
    } catch (...) {
      error = "IR profile work could not be paused.";
      return false;
    }
  }

  bool activateIrProfileServices(std::string_view profileId,
                                 const AppSettings &profileSettings,
                                 std::string &error) noexcept {
    try {
      std::string credentialDiagnostic;
      if (!prepareIrCredentials(profileId, credentialDiagnostic)) {
        SDL_Log("Authenticated IR is disabled for the selected profile");
      }
      std::string cacheDiagnostic;
      if (!bokutachiCacheStore->activate(
              profileManager.activePaths().bokutachiCacheJson,
              cacheDiagnostic)) {
        SDL_Log(
            "Bokutachi lookup cache was unavailable; using network lookups");
      }
      if (irRankingService) {
        irRankingService->activateProfile(profileId);
      }
      restoreMirroredIrScores(profileId, profileSettings);
      if (irSubmissionService) {
        irSubmissionService->activateProfile(
            irProfileConfig(profileId, profileSettings));
      }
      return true;
    } catch (...) {
      error = "IR profile work could not be activated.";
      return false;
    }
  }

  void setIrApplicationActive(bool active) noexcept {
    try {
      if (irSubmissionService) {
        irSubmissionService->setApplicationActive(active);
        if (active) {
          irSubmissionService->notifyOutboxChanged();
        }
      }
      if (!active && irRankingService) {
        const auto ranking = irRankingService->snapshot();
        irRankingService->close(ranking.generation);
      }
    } catch (...) {
      SDL_Log("IR application lifecycle update failed");
    }
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
    if (irSubmissionService) {
      irSubmissionService->stop();
    }
    if (irRankingService) {
      irRankingService->stop();
    }
    irSubmissionService.reset();
    irRankingService.reset();
    irHttpClient.reset();
    scoreRepository.Shutdown();
    replayRepository.Shutdown();
    std::cout << "Main function is quitting..." << std::endl;
  }
};
