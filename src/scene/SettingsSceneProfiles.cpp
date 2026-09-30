#include "../i18n/Localization.h"
#include "SettingsSceneShared.h"
#include "ProfileRuntimeReapply.h"

#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#endif
#include "../ProfileExportStaging.h"
#include "../audio/NativeMusicPlayer.h"
#include "../view/ScrollView.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

using namespace settings_scene;

namespace {
View *makeProfileCardsColumn(const LayoutMetrics &metrics) {
  auto *column = new View();
  column->setFlexDirection(FlexDirection::Column);
  column->setGap(static_cast<float>(metrics.secondaryGap));
  column->setWidth(static_cast<float>(metrics.cardsWidth));
  return column;
}

SDL_Color statusColor(ProfileSettingsStatusKind kind) {
  switch (kind) {
  case ProfileSettingsStatusKind::Success:
    return {157, 220, 176, 255};
  case ProfileSettingsStatusKind::Warning:
    return {255, 209, 128, 255};
  case ProfileSettingsStatusKind::Error:
    return {255, 177, 170, 255};
  case ProfileSettingsStatusKind::Info:
    return {185, 214, 255, 255};
  case ProfileSettingsStatusKind::None:
    return {157, 177, 200, 255};
  }
  return {157, 177, 200, 255};
}

Button *makeProfileActionButton(const LayoutMetrics &metrics,
                                const i18n::Text &label, bool enabled,
                                std::function<void()> action, int width = 0) {
  auto *text = makeText(label, metrics.bodyTextSize, ui_theme::textPrimary(),
                        TextView::CENTER, TextView::MIDDLE);
  auto *button =
      makeControlButton(width > 0 ? width : metrics.actionButtonWidth,
                        metrics.actionButtonHeight, text);
  button->setEnabled(enabled);
  if (enabled) {
    button->setOnClickListener(std::move(action));
  } else {
    text->setThemedColor(ui_theme::textMuted);
  }
  return button;
}

constexpr std::string_view kProfileArchiveMimeType = "application/zip";
constexpr std::string_view kProfileArchiveExportName =
    profile_export_staging::kArchiveName;

std::filesystem::path profileArchiveTemporaryRoot(std::string &errorMessage) {
#if TARGET_OS_ANDROID
  const std::string privateCache = GetAndroidCacheDir();
  if (privateCache.empty()) {
    errorMessage = i18n::tr("settings.profiles.android_private_storage_unavailable.message");
    return {};
  }
  return platform_document_handoff::detail::PathFromUtf8(privateCache);
#else
  std::error_code error;
  auto root = std::filesystem::temp_directory_path(error);
  if (error || root.empty()) {
    errorMessage =
        error ? i18n::tr("settings.profiles.unable_locate_private_temporary_storage.prefix") + error.message()
              : i18n::tr("settings.profiles.private_temporary_storage_unavailable.message");
    return {};
  }
  return root;
#endif
}

std::optional<profile_export_staging::Request>
profileExportStagingRequest(ApplicationContext &context,
                            std::string &errorMessage) {
  auto temporaryRoot = profileArchiveTemporaryRoot(errorMessage);
  if (temporaryRoot.empty()) {
    return std::nullopt;
  }
  return profile_export_staging::Request{
      .temporaryRoot = std::move(temporaryRoot),
      .managedApplicationRoot = context.applicationDataRoot,
      .reportWarning = [](const std::string &warning) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Profile export staging warning: %s", warning.c_str());
      }};
}

bool cleanupProfileImportTemporaryDocument(
    PlatformDocumentHandoffResult &temporaryDocument,
    std::string_view contextMessage) {
  if (platform_document_handoff::CleanupTemporaryDocument(temporaryDocument)) {
    return true;
  }
  SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "%.*s",
              static_cast<int>(contextMessage.size()), contextMessage.data());
  return false;
}

std::string joinWarnings(const std::vector<std::string> &warnings) {
  std::ostringstream message;
  for (const auto &warning : warnings) {
    if (warning.empty()) {
      continue;
    }
    if (message.tellp() > 0) {
      message << ' ';
    }
    message << warning;
  }
  return message.str();
}
} // namespace

void SettingsScene::ensureProfileController() {
  if (!profileExportStagingSwept) {
    profileExportStagingSwept = true;
    std::string requestError;
    try {
      auto request = profileExportStagingRequest(context, requestError);
      if (request) {
        const auto swept = profile_export_staging::Sweep(*request);
        if (!swept.ok()) {
          SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                      "Profile export staging sweep failed: %s",
                      swept.errorMessage.c_str());
        }
      } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Profile export staging sweep skipped: %s",
                    requestError.c_str());
      }
    } catch (const std::exception &error) {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                  "Profile export staging sweep failed: %s", error.what());
    } catch (...) {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                  "Profile export staging sweep failed.");
    }
  }
  if (profileController == nullptr) {
    profileController = std::make_unique<ProfileSettingsController>(context);
  }
}

void SettingsScene::invalidateProfileLayout() { lastLayoutWidth = -1; }

bool SettingsScene::startProfileArchiveTask(
    ProfileArchiveTask task,
    std::optional<PlatformDocumentHandoffResult> temporaryDocument) {
  ensureProfileController();
  if (profileController == nullptr || profileArchiveGeneration != 0 ||
      profileArchiveWorker.hasWorker()) {
    if (temporaryDocument) {
      cleanupProfileImportTemporaryDocument(
          *temporaryDocument,
          i18n::tr("settings.profiles.import.cleanup_before_start_failed"));
    }
    if (profileController != nullptr) {
      profileController->abandonArchive(task.generation());
      profileController->recordError(i18n::tr("settings.profiles.profile_task_already_running.message"));
    }
    invalidateProfileLayout();
    return false;
  }

  profileArchiveGeneration = task.generation();
  std::shared_ptr<PlatformDocumentHandoffResult> temporaryDocumentHolder;
  try {
    if (temporaryDocument) {
      temporaryDocumentHolder = std::make_shared<PlatformDocumentHandoffResult>(
          std::move(*temporaryDocument));
    }
    profileArchiveWorker.start(std::move(task),
        [temporaryDocument = temporaryDocumentHolder](ProfileArchiveResult &result) {
          const bool temporaryCleanupFailed =
              temporaryDocument &&
              !cleanupProfileImportTemporaryDocument(
                  *temporaryDocument,
                  i18n::tr("settings.profiles.import.cleanup_deferred"));
          if (temporaryCleanupFailed && result.ok()) {
            const std::string cleanupWarning =
                i18n::tr("settings.profiles.profile_imported_temporary_cleanup_pending.message");
            if (result.message.empty()) {
              result.message = cleanupWarning;
            } else {
              result.message += "; " + cleanupWarning;
            }
          }
        });
  } catch (const std::exception &error) {
    if (temporaryDocumentHolder) {
      cleanupProfileImportTemporaryDocument(
          *temporaryDocumentHolder,
          i18n::tr("settings.profiles.import.cleanup_after_start_failed"));
    } else if (temporaryDocument) {
      cleanupProfileImportTemporaryDocument(
          *temporaryDocument,
          i18n::tr("settings.profiles.import.cleanup_after_start_failed"));
    }
    profileController->abandonArchive(profileArchiveGeneration);
    profileArchiveGeneration = 0;
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "Unable to start profile archive worker: %s", error.what());
    profileController->recordError(i18n::tr("settings.profiles.could_not_start_profile_task.message"));
    invalidateProfileLayout();
    return false;
  } catch (...) {
    if (temporaryDocumentHolder) {
      cleanupProfileImportTemporaryDocument(
          *temporaryDocumentHolder,
          i18n::tr("settings.profiles.import.cleanup_after_start_failed"));
    } else if (temporaryDocument) {
      cleanupProfileImportTemporaryDocument(
          *temporaryDocument,
          i18n::tr("settings.profiles.import.cleanup_after_start_failed"));
    }
    profileController->abandonArchive(profileArchiveGeneration);
    profileArchiveGeneration = 0;
    profileController->recordError(i18n::tr("settings.profiles.could_not_start_profile_task.message"));
    invalidateProfileLayout();
    return false;
  }
  invalidateProfileLayout();
  return true;
}

void SettingsScene::startProfileImportDocumentPicker(
    const ProfileImportOptions &options, bool confirmedOverwrite) {
  ensureProfileController();
  if (profileController == nullptr) {
    return;
  }
  const bool began = confirmedOverwrite
                         ? profileController->beginConfirmedOverwritePicker()
                         : profileController->beginImportPicker();
  if (!began) {
    invalidateProfileLayout();
    return;
  }

  pendingProfileImportOptions = options;
  try {
    profileDocumentHandoff = platform_document_handoff::ImportDocumentAsync(
        {.mimeType = std::string(kProfileArchiveMimeType),
         .maxBytes = ProfileArchiveSizePolicy::kMaximumExistingArchiveBytes},
        context.temporaryPathCleanupService);
    if (!profileDocumentHandoff) {
      throw std::runtime_error(i18n::tr("settings.profiles.document_picker_did_not_start.message"));
    }
    profileDocumentHandoffKind = SettingsProfileDocumentHandoffKind::Import;
  } catch (const std::exception &error) {
    profileController->failPicker(i18n::tr("settings.profiles.unable_open_profile_import_picker.prefix") +
                                  std::string(error.what()));
    pendingProfileImportOptions = {};
    profileDocumentHandoff.close();
    profileDocumentHandoffKind = SettingsProfileDocumentHandoffKind::None;
  } catch (...) {
    profileController->failPicker(i18n::tr("settings.profiles.unable_open_profile_import_picker.message"));
    pendingProfileImportOptions = {};
    profileDocumentHandoff.close();
    profileDocumentHandoffKind = SettingsProfileDocumentHandoffKind::None;
  }
  invalidateProfileLayout();
}

void SettingsScene::startProfileExportPreparation(std::string_view profileId) {
  ensureProfileController();
  if (profileController == nullptr) {
    return;
  }

  std::string errorMessage;
  profile_export_staging::Result staging;
  try {
    auto request = profileExportStagingRequest(context, errorMessage);
    if (request) {
      staging = profile_export_staging::Create(*request);
      if (!staging.ok()) {
        errorMessage = std::move(staging.errorMessage);
      }
    }
  } catch (const std::exception &error) {
    errorMessage = i18n::tr("settings.profiles.unable_allocate_private_export_storage.prefix") +
                   std::string(error.what());
  } catch (...) {
    errorMessage = i18n::tr("settings.profiles.unable_allocate_private_export_storage.message");
  }
  if (!staging.ok()) {
    profileController->recordError(
        errorMessage.empty() ? i18n::tr("settings.profiles.unable_allocate_private_export_storage.message")
                             : std::move(errorMessage));
    invalidateProfileLayout();
    return;
  }

  auto task = profileController->beginExport(profileId, staging.archivePath);
  if (!task) {
    invalidateProfileLayout();
    return;
  }
  profileExportStagingFile = std::move(staging.archivePath);
  profileExportSourceLifetime = std::move(staging.sourceLifetime);
  if (!startProfileArchiveTask(std::move(*task))) {
    profileExportSourceLifetime.reset();
    profileExportStagingFile.clear();
  }
}

void SettingsScene::applyPendingProfileArchiveCompletion() {
  if (profileController == nullptr) return;
  auto completion = profileArchiveWorker.takeCompletion();
  if (!completion) return;

  if (completion->kind == ProfileArchiveTaskKind::Export &&
      completion->result.ok()) {
    if (!profileController->beginPreparedExportPicker(completion->generation)) {
      profileController->abandonArchive(completion->generation);
      profileController->recordError(i18n::tr("settings.profiles.could_not_open_save_picker.message"));
      profileArchiveGeneration = 0;
      profileExportSourceLifetime.reset();
      profileExportStagingFile.clear();
      invalidateProfileLayout();
      return;
    }

    try {
      PlatformDocumentExportRequest request{
          .localPath = profileExportStagingFile,
          .mimeType = std::string(kProfileArchiveMimeType),
          .suggestedName = std::string(kProfileArchiveExportName),
          .maxBytes = ProfileArchiveSizePolicy::kMaximumExistingArchiveBytes,
          .sourceLifetime = profileExportSourceLifetime};
      profileDocumentHandoff =
          platform_document_handoff::ExportDocumentAsync(std::move(request));
      if (!profileDocumentHandoff) {
        throw std::runtime_error(i18n::tr("settings.profiles.document_picker_did_not_start.message"));
      }
      preparedProfileExportResult = std::move(completion->result);
      profileDocumentHandoffKind = SettingsProfileDocumentHandoffKind::Export;
      // Detached native work now owns the temporary source lifetime. Releasing
      // the scene's copy is safe even if the scene closes before that work.
      profileExportSourceLifetime.reset();
      profileExportStagingFile.clear();
      invalidateProfileLayout();
      return;
    } catch (const std::exception &error) {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                  "Unable to open profile export picker: %s", error.what());
      profileController->failPicker(i18n::tr("settings.profiles.could_not_open_save_picker.message"));
    } catch (...) {
      profileController->failPicker(i18n::tr("settings.profiles.could_not_open_save_picker.message"));
    }
    profileDocumentHandoff.close();
    profileDocumentHandoffKind = SettingsProfileDocumentHandoffKind::None;
    preparedProfileExportResult.reset();
    profileArchiveGeneration = 0;
    profileExportSourceLifetime.reset();
    profileExportStagingFile.clear();
    invalidateProfileLayout();
    return;
  }

  profileArchiveGeneration = 0;
  if (!profileController->completeArchive(
          completion->kind, completion->generation, completion->result)) {
    profileController->abandonArchive(completion->generation);
  }
  if (completion->kind == ProfileArchiveTaskKind::Export) {
    profileExportSourceLifetime.reset();
    profileExportStagingFile.clear();
  }
  invalidateProfileLayout();
}

void SettingsScene::applyPendingProfileDocumentHandoff() {
  if (!profileDocumentHandoff || !profileDocumentHandoff.ready()) {
    return;
  }

  const auto kind = profileDocumentHandoffKind;
  auto result = profileDocumentHandoff.takeResult();
  profileDocumentHandoff.close();
  profileDocumentHandoffKind = SettingsProfileDocumentHandoffKind::None;
  if (profileController == nullptr) {
    return;
  }

  if (!result) {
    profileController->failPicker(i18n::tr("settings.profiles.file_picker_did_not_return_file.message"));
  } else if (result->cancelled()) {
    profileController->cancelPicker();
  } else if (!result->ok()) {
    std::string message = result->message;
    if (kind == SettingsProfileDocumentHandoffKind::Export) {
      message = i18n::tr("settings.profiles.could_not_save_profile.message") +
                (message.empty() ? std::string{} : " " + message);
    }
    profileController->failPicker(std::move(message));
  } else if (kind == SettingsProfileDocumentHandoffKind::Import) {
    const ProfileImportOptions options = pendingProfileImportOptions;
    pendingProfileImportOptions = {};
    auto task = profileController->beginImport(result->localPath, options);
    if (task) {
      startProfileArchiveTask(std::move(*task), std::move(*result));
    } else if (!platform_document_handoff::CleanupTemporaryDocument(*result)) {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                  "Rejected profile import temporary archive cleanup is "
                  "deferred; ownership cleanup will retry.");
    }
    invalidateProfileLayout();
    return;
  } else if (kind == SettingsProfileDocumentHandoffKind::Export &&
             preparedProfileExportResult && profileArchiveGeneration != 0) {
    const std::uint64_t generation = profileArchiveGeneration;
    profileArchiveGeneration = 0;
    if (!profileController->completeArchive(ProfileArchiveTaskKind::Export,
                                            generation,
                                            *preparedProfileExportResult)) {
      profileController->abandonArchive(generation);
    }
  } else {
    profileController->failPicker(i18n::tr("settings.profiles.unexpected_file_picker_result.message"));
  }

  pendingProfileImportOptions = {};
  if (kind == SettingsProfileDocumentHandoffKind::Export) {
    profileArchiveGeneration = 0;
    preparedProfileExportResult.reset();
  }
  invalidateProfileLayout();
}

void SettingsScene::stopProfileArchiveWork() {
  profileDocumentHandoff.close();
  profileDocumentHandoffKind = SettingsProfileDocumentHandoffKind::None;
  if (profileController != nullptr &&
      (profileController->phase() == ProfileSettingsPhase::PickingImport ||
       profileController->phase() == ProfileSettingsPhase::PickingExport)) {
    profileController->cancelPicker();
  }
  profileArchiveWorker.stopAndWait();
  if (profileController != nullptr && profileArchiveGeneration != 0) {
    profileController->abandonArchive(profileArchiveGeneration);
  }
  profileArchiveGeneration = 0;
  pendingProfileImportOptions = {};
  preparedProfileExportResult.reset();
  profileExportSourceLifetime.reset();
  profileExportStagingFile.clear();
  profileController.reset();
}

void SettingsScene::activateProfile(std::string_view profileId) {
  ensureProfileController();
  ensureAudioVideoSession();
  if (profileController == nullptr) {
    return;
  }

  const ProfileSwitchResult switched = profileController->activate(profileId);
  // The profile commit is authoritative. This ordered runtime pass is
  // best-effort and never rolls the committed profile back.
  const ProfileRuntimeReapplyResult runtime = ReapplyProfileRuntimeAfterSwitch(
      switched,
      {.sanitize = [this]() { context.settings.sanitize(); },
       .applyTheme =
           [this]() {
             ui_theme::setActiveMode(context.settings.uiThemeMode ==
                                             AppSettings::UiThemeMode::Light
                                         ? ui_theme::ThemeMode::Light
                                         : ui_theme::ThemeMode::Dark);
           },
       .applyJukebox =
           [this]() {
             context.jukebox.setVisualsEnabled(context.settings.bgaEnabled);
             context.jukebox.setBgaOffsetMs(context.settings.audioOffsetMs);
             context.jukebox.setBgaDisplayMode(context.settings.bgaDisplayMode);
           },
       .applyMetadata =
           [this]() {
             std::string error;
             if (native_music_player::SetMetadataVisibility(
                     {.showTitle = context.settings.systemPlaybackShowTitle,
                      .showArtist = context.settings.systemPlaybackShowArtist,
                      .showArtwork = context.settings.systemPlaybackShowJacket},
                     error)) {
               return std::string{};
             }
             return error.empty()
                        ? i18n::tr("settings.profiles.system_media_metadata_preference_not_applied.message")
                        : i18n::tr("settings.profiles.system_media_metadata.prefix") + error;
           },
       .applyAudio =
           [this]() {
             const audio::ApplyResult result = context.audioDeviceManager.apply(
                 context.settings.audioVideo.audio);
             setAudioStatus(
                 result.message.empty()
                     ? (result.status == audio::ApplyStatus::Applied
                            ? i18n::tr("settings.profiles.profile_audio_settings_applied.message")
                            : i18n::tr("settings.profiles.profile_audio_settings_need_attention.message"))
                     : result.message,
                 result.status == audio::ApplyStatus::Applied
                     ? SDL_Color{157, 220, 176, 255}
                     : SDL_Color{255, 177, 170, 255});
             if (result.status == audio::ApplyStatus::Applied) {
               return std::string{};
             }
             return result.message.empty()
                        ? i18n::tr("settings.profiles.saved_audio_runtime_failed_fully_applied.message")
                        : i18n::tr("settings.profiles.audio.prefix") + result.message;
           },
       .refreshDrafts =
           [this]() {
             audioDraft = context.settings.audioVideo.audio;
             displayDraft = context.settings.audioVideo.video;
           },
       .applyDisplay =
           [this]() {
             if (audioVideoSession == nullptr) {
               return ProfileDisplayRuntimeResult{
                   .outcome = ProfileDisplayRuntimeOutcome::Failed,
                   .message = i18n::tr("settings.profiles.display_runtime_not_initialized_yet.message")};
             }
             const display::ApplyResult result =
                 audioVideoSession->beginDisplayPreview(
                     displayDraft, std::chrono::steady_clock::now());
             const bool accepted =
                 result.status == display::ApplyStatus::Applied ||
                 result.status == display::ApplyStatus::PreviewPending;
             setDisplayStatus(result.message.empty()
                                  ? (accepted
                                         ? i18n::tr("settings.profiles.profile_display_settings_applied.message")
                                         : i18n::tr("settings.profiles.profile_display_settings_need_attention.message"))
                                  : result.message,
                              accepted ? SDL_Color{157, 220, 176, 255}
                                       : SDL_Color{255, 177, 170, 255});
             updateDisplayPreviewUi();
             if (result.status == display::ApplyStatus::PreviewPending) {
               return ProfileDisplayRuntimeResult{
                   .outcome = ProfileDisplayRuntimeOutcome::PreviewPending,
                   .message = result.message};
             }
             return ProfileDisplayRuntimeResult{
                 .outcome = result.status == display::ApplyStatus::Applied
                                ? ProfileDisplayRuntimeOutcome::Applied
                                : ProfileDisplayRuntimeOutcome::Failed,
                 .message =
                     accepted || !result.message.empty()
                         ? result.message
                         : i18n::tr("settings.profiles.saved_display_runtime_failed_fully_applied.message")};
           }});

  const std::string warningText = joinWarnings(runtime.warnings);
  if (runtime.profileCommitted && !warningText.empty()) {
    profileController->recordWarning(
        i18n::tr("settings.profiles.profile_switched_warnings.prefix") + warningText);
  }
  invalidateProfileLayout();
}

View *SettingsScene::buildProfileTab(const LayoutMetrics &metrics) {
  ensureProfileController();
  auto *cardsColumn = makeProfileCardsColumn(metrics);
  if (profileController == nullptr) {
    cardsColumn->addView(makeCard(
        metrics, i18n::message("settings.profiles.player_profiles.label"), i18n::message("settings.profiles.profile_services_unavailable.message"),
        makeWrappedText(i18n::message("settings.profiles.restart_app_try_again.message"),
                        metrics.bodyTextSize, ui_theme::textSecondary()),
        metrics.modeCardHeight, metrics.cardsWidth));
    return cardsColumn;
  }

  const bool idle = profileController->actionsEnabled();
  const auto phase = profileController->phase();

  const bool editorTargetAvailable =
      !profileInlineEditor.active() ||
      std::ranges::any_of(profileController->profiles(), [&](const auto &item) {
        return profileInlineEditor.activeFor(item.id);
      });
  profileInlineEditor.clearIfUnavailable(editorTargetAvailable, idle);

  const auto &status = profileController->status();
  if (!status.message.empty()) {
    profileStatusText = makeWrappedText(
        status.message, metrics.bodyTextSize, ui_theme::textSecondary());
    profileStatusText->setColor(statusColor(status.kind));
    cardsColumn->addView(profileStatusText);
  }

  auto *manageBody = new View();
  manageBody->setFlexDirection(FlexDirection::Column);
  manageBody->setGap(metrics.compact ? 10.0f : 14.0f);

  profileCreateNameInput =
      makeTextInput(metrics, std::max(260, metrics.cardsWidth / 2));
  profileCreateNameInput->setEditingText(profileCreateNameText);
  profileCreateNameInput->onTextChanged(
      [this](const std::string &text) { profileCreateNameText = text; });
  manageBody->addView(profileCreateNameInput);

  auto *manageActions = new View();
  manageActions->setFlexDirection(FlexDirection::Row);
  manageActions->setFlexWrap(YGWrapWrap);
  manageActions->setGap(metrics.compact ? 8.0f : 10.0f);
  manageActions->addView(
      makeProfileActionButton(metrics, i18n::message("settings.profiles.create.label"), idle, [this]() {
        profileController->create(profileCreateNameText);
        invalidateProfileLayout();
      }));
  manageBody->addView(manageActions);
  cardsColumn->addView(makeCard(
      metrics, i18n::message("settings.profiles.player_profiles.label"), i18n::message("settings.profiles.keep_settings_records_separate.message"),
      manageBody, metrics.modeCardHeight, metrics.cardsWidth));

  auto *archiveBody = new View();
  archiveBody->setFlexDirection(FlexDirection::Column);
  archiveBody->setGap(metrics.compact ? 10.0f : 14.0f);
  auto *archiveActions = new View();
  archiveActions->setFlexDirection(FlexDirection::Row);
  archiveActions->setFlexWrap(YGWrapWrap);
  archiveActions->setGap(metrics.compact ? 8.0f : 10.0f);
  archiveActions->addView(
      makeProfileActionButton(metrics, i18n::message("settings.profiles.import.label"), idle, [this]() {
        startProfileImportDocumentPicker(
            {.mode = ProfileImportMode::CreateWithNewId});
      }));
  archiveBody->addView(archiveActions);

  cardsColumn->addView(makeCard(
      metrics, "Import / Export", i18n::message("settings.profiles.move_profiles_between_devices.message"),
      archiveBody, metrics.modeCardHeight, metrics.cardsWidth));

  for (const PlayerProfile &profile : profileController->profiles()) {
    const bool selected = profile.id == profileController->selectedProfileId();
    const bool active = profile.id == profileController->activeProfileId();
    const bool confirmingDelete =
        phase == ProfileSettingsPhase::ConfirmDelete &&
        profile.id == profileController->confirmationProfileId();
    const bool confirmingOverwrite =
        phase == ProfileSettingsPhase::ConfirmOverwrite &&
        profile.id == profileController->confirmationProfileId();
    const auto deleteEligibility =
        profileController->deleteEligibility(profile.id);
    const auto overwriteEligibility =
        profileController->overwriteEligibility(profile.id);

    auto *body = new View();
    body->setFlexDirection(FlexDirection::Column);
    body->setGap(metrics.compact ? 9.0f : 12.0f);
    body->addView(makeWrappedText(
        i18n::message("settings.profiles.profile.summary",
                      {{"active", active ? i18n::message("settings.profiles.active.prefix")
                                           : i18n::Text("")},
                       {"selected", selected ? i18n::message("settings.profiles.selected.prefix")
                                               : i18n::Text("")},
                       {"lastUsed", i18n::message("settings.profiles.last_used.prefix")},
                       {"timestamp", profile.lastUsedAt}}),
        metrics.smallTextSize,
        active ? ui_theme::lime() : ui_theme::textSecondary()));

    auto *actions = new View();
    actions->setFlexDirection(FlexDirection::Row);
    actions->setFlexWrap(YGWrapWrap);
    actions->setGap(metrics.compact ? 8.0f : 10.0f);
    actions->addView(makeProfileActionButton(
        metrics, selected ? i18n::message("settings.profiles.selected.label") : i18n::message("settings.profiles.select.label"), idle && !selected,
        [this, id = profile.id]() {
          profileController->select(id);
          invalidateProfileLayout();
        }));
    actions->addView(makeProfileActionButton(
        metrics, active ? i18n::message("settings.profiles.active.label") : i18n::message("settings.profiles.activate.label"), idle && !active,
        [this, id = profile.id]() { activateProfile(id); }));
    actions->addView(makeProfileActionButton(
        metrics, i18n::message("settings.profiles.rename.label"), idle,
        [this, id = profile.id, name = profile.displayName]() {
          profileInlineEditor.beginRename(id, name);
          invalidateProfileLayout();
        }));
    actions->addView(makeProfileActionButton(
        metrics, i18n::message("settings.profiles.copy.label"), idle,
        [this, id = profile.id, name = profile.displayName]() {
          profileInlineEditor.beginDuplicate(id, name);
          invalidateProfileLayout();
        }));
    actions->addView(makeProfileActionButton(
        metrics, confirmingDelete ? i18n::message("settings.profiles.confirm_delete.label") : i18n::message("settings.profiles.delete.label"),
        confirmingDelete || (idle && deleteEligibility.enabled),
        [this, id = profile.id, confirmingDelete]() {
          if (confirmingDelete) {
            profileController->confirmDelete();
          } else {
            profileController->requestDelete(id);
          }
          invalidateProfileLayout();
        }));
    actions->addView(makeProfileActionButton(
        metrics, i18n::message("settings.profiles.export.label"), idle,
        [this, id = profile.id]() { startProfileExportPreparation(id); }));
    actions->addView(makeProfileActionButton(
        metrics,
        confirmingOverwrite ? i18n::message("settings.profiles.confirm_import.label") : i18n::message("settings.profiles.import_over.label"),
        confirmingOverwrite || (idle && overwriteEligibility.enabled),
        [this, id = profile.id, confirmingOverwrite]() {
          if (!confirmingOverwrite) {
            profileController->requestOverwrite(id);
            invalidateProfileLayout();
            return;
          }
          startProfileImportDocumentPicker(
              {.mode = ProfileImportMode::Overwrite, .overwriteProfileId = id},
              true);
        }));

    if (confirmingDelete || confirmingOverwrite) {
      body->addView(makeWrappedText(
          confirmingDelete
              ? i18n::message("settings.profiles.delete_profile.label")
              : i18n::message("settings.profiles.replace_profile.label"),
          metrics.bodyTextSize, ui_theme::amber()));
      actions->addView(makeProfileActionButton(
          metrics, i18n::message("settings.profiles.cancel.label"), true, [this]() {
            profileController->cancelConfirmation();
            invalidateProfileLayout();
          }));
    }
    body->addView(actions);

    if (profileInlineEditor.activeFor(profile.id)) {
      auto *editorInput =
          makeTextInput(metrics, std::max(260, metrics.cardsWidth / 2));
      editorInput->setEditingText(std::string(profileInlineEditor.draft()));
      editorInput->onTextChanged(
          [this, id = profile.id](const std::string &text) {
            if (profileInlineEditor.activeFor(id)) {
              profileInlineEditor.updateDraft(text);
            }
          });
      body->addView(editorInput);

      auto *editorActions = new View();
      editorActions->setFlexDirection(FlexDirection::Row);
      editorActions->setFlexWrap(YGWrapWrap);
      editorActions->setGap(metrics.compact ? 8.0F : 10.0F);
      editorActions->addView(makeProfileActionButton(
          metrics, i18n::message("settings.profiles.apply.label"), idle, [this, id = profile.id]() {
            const auto request = profileInlineEditor.requestFor(id);
            if (!request) {
              return;
            }
            const ProfileResult result =
                request->action ==
                        settings_scene::ProfileInlineEditAction::Rename
                    ? profileController->rename(request->profileId,
                                                request->name)
                    : profileController->duplicate(request->profileId,
                                                   request->name);
            if (result.ok()) {
              profileInlineEditor.clear();
            }
            invalidateProfileLayout();
          }));
      editorActions->addView(makeProfileActionButton(
          metrics, i18n::message("settings.profiles.cancel.label"), true, [this]() {
            profileInlineEditor.clear();
            invalidateProfileLayout();
          }));
      body->addView(editorActions);
    }

    std::string disabledReason;
    if (!deleteEligibility.enabled && !confirmingDelete) {
      disabledReason = deleteEligibility.reason;
    } else if (!overwriteEligibility.enabled && !confirmingOverwrite) {
      disabledReason = overwriteEligibility.reason;
    }
    if (!disabledReason.empty()) {
      auto *reason = makeWrappedText(disabledReason, metrics.smallTextSize,
                                     ui_theme::textMuted());
      if (selected) {
        profileDeleteReasonText = reason;
      }
      body->addView(reason);
    }

    cardsColumn->addView(makeCard(
        metrics, profile.displayName, "",
        body, metrics.modeCardHeight, metrics.cardsWidth));
  }
  return cardsColumn;
}
