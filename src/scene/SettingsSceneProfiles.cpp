#include "SettingsSceneShared.h"
#include "ProfileRuntimeReapply.h"

#include "../audio/NativeMusicPlayer.h"
#include "../view/ScrollView.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <sstream>
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

std::string abbreviatedUuid(std::string_view id) {
  if (id.size() <= 13) {
    return std::string(id);
  }
  return std::string(id.substr(0, 8)) + "…" +
         std::string(id.substr(id.size() - 4));
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
                                const std::string &label, bool enabled,
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

bool archivePipelinePhase(ProfileSettingsPhase phase) {
  return phase == ProfileSettingsPhase::PickingImport ||
         phase == ProfileSettingsPhase::Importing ||
         phase == ProfileSettingsPhase::PreparingExport ||
         phase == ProfileSettingsPhase::PickingExport;
}

std::string defaultArchivePath(const ApplicationContext &context) {
  std::filesystem::path directory = context.applicationDataRoot.parent_path();
  if (directory.empty() || directory == context.applicationDataRoot) {
    std::error_code error;
    directory = std::filesystem::temp_directory_path(error);
    if (error) {
      directory = ".";
    }
  }
  return (directory / "AsoBMaShow-profile.asobprofile").string();
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
  if (profileController == nullptr) {
    profileController = std::make_unique<ProfileSettingsController>(context);
  }
  if (profileArchiveMailbox == nullptr) {
    profileArchiveMailbox = std::make_shared<SettingsProfileArchiveMailbox>();
  }
  if (profileArchivePathText.empty()) {
    profileArchivePathText = defaultArchivePath(context);
  }
}

void SettingsScene::invalidateProfileLayout() { lastLayoutWidth = -1; }

void SettingsScene::startProfileArchiveTask(ProfileArchiveTask task) {
  ensureProfileController();
  if (profileController == nullptr || profileArchiveMailbox == nullptr ||
      profileArchiveGeneration != 0 || profileArchiveThread.joinable()) {
    if (profileController != nullptr) {
      profileController->abandonArchive(task.generation());
      profileController->recordWarning(
          "The profile archive worker is already busy.");
    }
    invalidateProfileLayout();
    return;
  }

  profileArchiveGeneration = task.generation();
  const auto mailbox = profileArchiveMailbox;
  try {
    profileArchiveThread =
        std::jthread([mailbox, task = std::move(task)](
                         const std::stop_token &stopToken) mutable {
          SettingsProfileArchiveCompletion completion{.kind = task.kind(),
                                                      .generation =
                                                          task.generation(),
                                                      .result = task.execute()};
          if (stopToken.stop_requested()) {
            return;
          }
          std::lock_guard<std::mutex> lock(mailbox->mutex);
          mailbox->completion = std::move(completion);
        });
  } catch (const std::exception &error) {
    profileController->abandonArchive(profileArchiveGeneration);
    profileArchiveGeneration = 0;
    profileController->recordWarning(
        "Unable to start the profile archive worker: " +
        std::string(error.what()));
  } catch (...) {
    profileController->abandonArchive(profileArchiveGeneration);
    profileArchiveGeneration = 0;
    profileController->recordWarning(
        "Unable to start the profile archive worker.");
  }
  invalidateProfileLayout();
}

void SettingsScene::applyPendingProfileArchiveCompletion() {
  if (profileArchiveMailbox == nullptr || profileController == nullptr) {
    return;
  }
  std::optional<SettingsProfileArchiveCompletion> completion;
  {
    std::lock_guard<std::mutex> lock(profileArchiveMailbox->mutex);
    if (!profileArchiveMailbox->completion) {
      return;
    }
    completion = std::move(profileArchiveMailbox->completion);
    profileArchiveMailbox->completion.reset();
  }
  if (profileArchiveThread.joinable()) {
    profileArchiveThread.join();
  }
  profileArchiveGeneration = 0;
  if (!profileController->completeArchive(
          completion->kind, completion->generation, completion->result)) {
    profileController->abandonArchive(completion->generation);
  }
  invalidateProfileLayout();
}

void SettingsScene::stopProfileArchiveWork() {
  if (profileArchiveThread.joinable()) {
    profileArchiveThread.request_stop();
    profileArchiveThread.join();
  }
  if (profileController != nullptr && profileArchiveGeneration != 0) {
    profileController->abandonArchive(profileArchiveGeneration);
  }
  profileArchiveGeneration = 0;
  if (profileArchiveMailbox != nullptr) {
    std::lock_guard<std::mutex> lock(profileArchiveMailbox->mutex);
    profileArchiveMailbox->completion.reset();
  }
  profileController.reset();
  profileArchiveMailbox.reset();
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
                        ? "The system media metadata preference was not "
                          "applied."
                        : "System media metadata: " + error;
           },
       .applyAudio =
           [this]() {
             const audio::ApplyResult result = context.audioDeviceManager.apply(
                 context.settings.audioVideo.audio);
             setAudioStatus(
                 result.message.empty()
                     ? (result.status == audio::ApplyStatus::Applied
                            ? "Profile audio settings applied."
                            : "Profile audio settings need attention.")
                     : result.message,
                 result.status == audio::ApplyStatus::Applied
                     ? SDL_Color{157, 220, 176, 255}
                     : SDL_Color{255, 177, 170, 255});
             if (result.status == audio::ApplyStatus::Applied) {
               return std::string{};
             }
             return result.message.empty()
                        ? "The saved audio runtime could not be fully "
                          "applied."
                        : "Audio: " + result.message;
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
                   .message = "The display runtime is not initialized yet."};
             }
             const display::ApplyResult result =
                 audioVideoSession->beginDisplayPreview(
                     displayDraft, std::chrono::steady_clock::now());
             const bool accepted =
                 result.status == display::ApplyStatus::Applied ||
                 result.status == display::ApplyStatus::PreviewPending;
             setDisplayStatus(result.message.empty()
                                  ? (accepted
                                         ? "Profile display settings applied."
                                         : "Profile display settings need "
                                           "attention.")
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
                         : "The saved display runtime could not be fully "
                           "applied."};
           }});

  const std::string warningText = joinWarnings(runtime.warnings);
  if (runtime.profileCommitted && !warningText.empty()) {
    profileController->recordWarning(
        "Profile switched, but some runtime settings need attention. " +
        warningText);
  }
  invalidateProfileLayout();
}

View *SettingsScene::buildProfileTab(const LayoutMetrics &metrics) {
  ensureProfileController();
  auto *cardsColumn = makeProfileCardsColumn(metrics);
  if (profileController == nullptr) {
    cardsColumn->addView(makeCard(
        metrics, "Player Profiles", "Profile services are unavailable.",
        makeWrappedText("Restart the application after checking the profile "
                        "data directory.",
                        metrics.bodyTextSize, ui_theme::textSecondary()),
        metrics.modeCardHeight, metrics.cardsWidth));
    return cardsColumn;
  }

  const bool idle = profileController->actionsEnabled();
  const auto phase = profileController->phase();

  auto *manageBody = new View();
  manageBody->setFlexDirection(FlexDirection::Column);
  manageBody->setGap(metrics.compact ? 10.0f : 14.0f);

  profileNameInput =
      makeTextInput(metrics, std::max(260, metrics.cardsWidth / 2));
  profileNameInput->setEditingText(profileNameText);
  profileNameInput->onTextChanged(
      [this](const std::string &text) { profileNameText = text; });
  manageBody->addView(profileNameInput);

  auto *manageActions = new View();
  manageActions->setFlexDirection(FlexDirection::Row);
  manageActions->setFlexWrap(YGWrapWrap);
  manageActions->setGap(metrics.compact ? 8.0f : 10.0f);
  manageActions->addView(
      makeProfileActionButton(metrics, "Create", idle, [this]() {
        profileController->create(profileNameText);
        invalidateProfileLayout();
      }));
  manageActions->addView(makeProfileActionButton(
      metrics, "Cancel Confirmation",
      phase == ProfileSettingsPhase::ConfirmDelete ||
          phase == ProfileSettingsPhase::ConfirmOverwrite,
      [this]() {
        profileController->cancelConfirmation();
        invalidateProfileLayout();
      }));
  manageBody->addView(manageActions);
  cardsColumn->addView(makeCard(
      metrics, "Player Profiles",
      "Create isolated settings, input, score, and replay identities. New and "
      "duplicated profiles are selected but never activated automatically.",
      manageBody, metrics.modeCardHeight, metrics.cardsWidth));

  auto *archiveBody = new View();
  archiveBody->setFlexDirection(FlexDirection::Column);
  archiveBody->setGap(metrics.compact ? 10.0f : 14.0f);
  profileArchivePathInput =
      makeTextInput(metrics, std::max(300, metrics.cardsWidth / 2));
  profileArchivePathInput->setEditingText(profileArchivePathText);
  profileArchivePathInput->onTextChanged(
      [this](const std::string &text) { profileArchivePathText = text; });
  archiveBody->addView(profileArchivePathInput);
  archiveBody->addView(makeWrappedText(
      "Temporary path-based handoff for desktop/testing. Native document "
      "pickers plug into the same Picking Import/Picking Export controller "
      "states.",
      metrics.smallTextSize, ui_theme::textMuted()));

  auto *archiveActions = new View();
  archiveActions->setFlexDirection(FlexDirection::Row);
  archiveActions->setFlexWrap(YGWrapWrap);
  archiveActions->setGap(metrics.compact ? 8.0f : 10.0f);
  archiveActions->addView(
      makeProfileActionButton(metrics, "Import New", idle, [this]() {
        auto task = profileController->beginImport(
            std::filesystem::path(profileArchivePathText),
            {.mode = ProfileImportMode::CreateWithNewId});
        if (task) {
          startProfileArchiveTask(std::move(*task));
        } else {
          invalidateProfileLayout();
        }
      }));
  archiveBody->addView(archiveActions);

  const auto &status = profileController->status();
  profileStatusText =
      makeWrappedText(status.message.empty() ? "Ready." : status.message,
                      metrics.bodyTextSize, ui_theme::textSecondary());
  profileStatusText->setColor(statusColor(status.kind));
  archiveBody->addView(profileStatusText);
  cardsColumn->addView(makeCard(
      metrics, "Portable Archive",
      "Export or import the six-file allowlisted profile archive. Chart "
      "libraries and online services are never included.",
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
        std::string(active ? "ACTIVE  •  " : "") +
            (selected ? "SELECTED  •  " : "") + "Last used " +
            profile.lastUsedAt + "  •  " + abbreviatedUuid(profile.id),
        metrics.smallTextSize,
        active ? ui_theme::lime() : ui_theme::textSecondary()));

    auto *actions = new View();
    actions->setFlexDirection(FlexDirection::Row);
    actions->setFlexWrap(YGWrapWrap);
    actions->setGap(metrics.compact ? 8.0f : 10.0f);
    actions->addView(makeProfileActionButton(
        metrics, selected ? "Selected" : "Select",
        !selected && !archivePipelinePhase(phase), [this, id = profile.id]() {
          profileController->select(id);
          invalidateProfileLayout();
        }));
    actions->addView(makeProfileActionButton(
        metrics, active ? "Active" : "Activate", idle && !active,
        [this, id = profile.id]() { activateProfile(id); }));
    actions->addView(makeProfileActionButton(
        metrics, "Rename", idle, [this, id = profile.id]() {
          profileController->rename(id, profileNameText);
          invalidateProfileLayout();
        }));
    actions->addView(makeProfileActionButton(
        metrics, "Duplicate", idle, [this, id = profile.id]() {
          profileController->duplicate(id, profileNameText);
          invalidateProfileLayout();
        }));
    actions->addView(makeProfileActionButton(
        metrics, confirmingDelete ? "Confirm Delete" : "Delete",
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
        metrics, "Export", idle, [this, id = profile.id]() {
          auto task = profileController->beginExport(
              id, std::filesystem::path(profileArchivePathText));
          if (task) {
            startProfileArchiveTask(std::move(*task));
          } else {
            invalidateProfileLayout();
          }
        }));
    actions->addView(makeProfileActionButton(
        metrics,
        confirmingOverwrite ? "Confirm Overwrite" : "Overwrite from Archive",
        confirmingOverwrite || (idle && overwriteEligibility.enabled),
        [this, id = profile.id, confirmingOverwrite]() {
          if (!confirmingOverwrite) {
            profileController->requestOverwrite(id);
            invalidateProfileLayout();
            return;
          }
          auto task = profileController->beginImport(
              std::filesystem::path(profileArchivePathText),
              {.mode = ProfileImportMode::Overwrite, .overwriteProfileId = id});
          if (task) {
            startProfileArchiveTask(std::move(*task));
          } else {
            invalidateProfileLayout();
          }
        }));
    body->addView(actions);

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
        metrics, profile.displayName,
        active ? "This profile currently owns the live settings and database "
                 "session."
               : "Activate to bind this profile's settings, input, scores, "
                 "and replays.",
        body, metrics.modeCardHeight, metrics.cardsWidth));
  }
  return cardsColumn;
}
