#include "../i18n/Localization.h"
#include "SettingsSceneShared.h"
#include "../ArchiveFile.h"
#include "../library/ChartLibraryPlatform.h"
#include "../input/InputCaptureController.h"
#include "../input/RhythmInputHandler.h"
#include "../view/ScrollView.h"
#include "play/BMSRenderer.h"
#include "play/RhythmLaneInputController.h"

#include <iomanip>
#include <sstream>

using namespace settings_scene;

namespace {
std::string formatCacheBytes(std::uint64_t bytes) {
  constexpr double kib = 1024.0;
  constexpr double mib = kib * 1024.0;
  constexpr double gib = mib * 1024.0;
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(bytes >= 10 * 1024 ? 1 : 0);
  if (bytes >= static_cast<std::uint64_t>(gib)) {
    stream << static_cast<double>(bytes) / gib << " GB";
  } else if (bytes >= static_cast<std::uint64_t>(mib)) {
    stream << static_cast<double>(bytes) / mib << " MB";
  } else if (bytes >= static_cast<std::uint64_t>(kib)) {
    stream << static_cast<double>(bytes) / kib << " KB";
  } else {
    stream.unsetf(std::ios::floatfield);
    stream << bytes << " B";
  }
  return stream.str();
}

i18n::Text formatCacheCleanupResult(
    const archive_file::TemporaryCacheCleanupResult &result) {
  if (!result.cacheExisted || result.removedEntries == 0) {
    return result.skippedEntries == 0
               ? i18n::message("settings.temporary_archive_cache_already_empty.message")
               : i18n::message("settings.temporary_archive_cache_only_contains_active_files.message");
  }
  const auto summary = i18n::message(
      "settings.cache.cleanup.summary",
      {{"size", formatCacheBytes(result.removedBytes)},
       {"count", std::to_string(result.removedEntries)}});
  if (result.skippedEntries > 0) {
    return i18n::message(
        "settings.cache.cleanup.result",
        {{"summary", summary},
         {"skipped", i18n::message(
             result.skippedEntries == 1 ? "settings.cache.cleanup.skipped.one"
                                        : "settings.cache.cleanup.skipped.other",
             {{"count", std::to_string(result.skippedEntries)}})}});
  }
  return summary;
}

i18n::Text
formatCacheUsageResult(const archive_file::TemporaryCacheUsageResult &result) {
  if (!result.cacheExisted || result.entries == 0) {
    return i18n::message("settings.temporary_archive_cache_empty.message");
  }
  return i18n::message("settings.cache.usage.summary",
                      {{"size", formatCacheBytes(result.bytes)},
                       {"count", std::to_string(result.entries)}});
}
} // namespace

SettingsScene::SettingsScene(ApplicationContext &context,
                             SettingsDestination destination,
                             SceneReturnTarget returnTarget)
    : Scene(context), returnTarget_(std::move(returnTarget)),
      activeTab(destination == SettingsDestination::Ir ? SettingsTab::Ir
                                                       : SettingsTab::Profile),
      archiveCacheMaintenance(
          [&jukebox = context.jukebox](auto &result, auto &error) {
            const auto protectedPaths = jukebox.activeMaterializedVideoPaths();
            return archive_file::cleanupTemporaryCache(result, protectedPaths, &error);
          },
          [](auto &result, auto &error, const std::stop_token &token) {
            return archive_file::measureTemporaryCache(result, &error, &token);
          }),
      lastLaidOutTab(activeTab) {}

void SettingsScene::applyPendingArchiveCacheCleanupStatus() {
  const auto completion = archiveCacheMaintenance.takeCompletion();
  if (!completion) {
    return;
  }
  const bool cleanup =
      completion->operation == SettingsCacheMaintenance::Operation::Cleanup;
  if (!completion->succeeded) {
    archiveCacheCleanupStatusMessage = i18n::message(
        "settings.cache.operation.failure",
        {{"operation", i18n::message(
             cleanup ? "settings.archive_cache_cleanup_failed.label"
                     : "settings.archive_cache_measurement_failed.label")},
         {"details", completion->error.empty() ? "." : ": " + completion->error}});
    archiveCacheCleanupStatusColor = {255, 177, 170, 255};
  } else if (cleanup) {
    archiveCacheCleanupStatusMessage = formatCacheCleanupResult(completion->cleanup);
    archiveCacheCleanupStatusColor = {181, 228, 165, 255};
  } else {
    archiveCacheCleanupStatusMessage = formatCacheUsageResult(completion->usage);
    archiveCacheCleanupStatusColor = {157, 177, 200, 255};
  }

  if (archiveCacheCleanupStatusText != nullptr) {
    archiveCacheCleanupStatusText->setLocalizedText(archiveCacheCleanupStatusMessage);
    archiveCacheCleanupStatusText->setColor(archiveCacheCleanupStatusColor);
  }
  if (archiveCacheCleanupButtonText != nullptr) {
    archiveCacheCleanupButtonText->setLocalizedText(
        archiveCacheMaintenance.cleanupRunning() ? i18n::message("settings.cleaning.progress") : i18n::message("settings.clean_up.label"));
  }
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
  if (scrollView != nullptr) {
    scrollView->refreshContentLayout();
  }
}

void SettingsScene::cleanupTemporaryArchiveCache() {
  if (!archiveCacheMaintenance.startCleanup()) {
    return;
  }

  archiveCacheCleanupStatusMessage = i18n::message("settings.cleaning_temporary_archive_cache.progress");
  archiveCacheCleanupStatusColor = {239, 244, 251, 255};
  if (archiveCacheCleanupStatusText != nullptr) {
    archiveCacheCleanupStatusText->setLocalizedText(archiveCacheCleanupStatusMessage);
    archiveCacheCleanupStatusText->setColor(archiveCacheCleanupStatusColor);
  }
  if (archiveCacheCleanupButtonText != nullptr) {
    archiveCacheCleanupButtonText->setLocalizedText(i18n::message("settings.cleaning.progress"));
  }
}

void SettingsScene::measureTemporaryArchiveCache() {
  if (!archiveCacheMaintenance.startMeasure()) {
    return;
  }

  archiveCacheCleanupStatusMessage = i18n::message("settings.measuring_temporary_archive_cache.progress");
  archiveCacheCleanupStatusColor = {239, 244, 251, 255};
  if (archiveCacheCleanupStatusText != nullptr) {
    archiveCacheCleanupStatusText->setLocalizedText(archiveCacheCleanupStatusMessage);
    archiveCacheCleanupStatusText->setColor(archiveCacheCleanupStatusColor);
  }
}

void SettingsScene::init() {
  lastLayoutWidth = -1;
  ensureProfileController();
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  ensureGameplaySkinSettingsController();
  if (activeTab == SettingsTab::GameplaySkins) {
    updateGameplaySkinSettingsController();
  }
#endif
  ensureAudioVideoSession();
  context.profileSwitchBlockers.scene = [this]() -> std::optional<std::string> {
    if (audioVideoSession != nullptr &&
        audioVideoSession->hasDisplayPreview()) {
      return i18n::tr("settings.profile_switch.display_preview_blocker");
    }
    if (libraryTask.running()) {
      return i18n::tr("settings.difficulty_table_library_update_active.message");
    }
    if (archiveCacheMaintenance.running()) {
      return i18n::tr("settings.archive_cache_maintenance_active.message");
    }
    return std::nullopt;
  };
  ensureInputCaptureController();
  inputProfileReplacementRegistration =
      context.inputProfileReplacementNotifier.subscribe([this]() {
        if (inputCaptureController != nullptr) {
          inputCaptureController->cancel();
        }
        inputCaptureAction.reset();
        inputGyroscopeAxisValue = 0.0F;
        inputGyroscopeSettingsError.clear();
        inputViewRebuildGate.prepareForProfileReplacement();
      });
  observedLibraryRevision = context.chartRepository.GetLibraryRevision();
  ensureLayoutUpToDate();
}

void SettingsScene::onPresentationOrientationWillChange() {
  const auto finishSelected = [&](auto &&self, View *view) -> bool {
    if (auto *input = dynamic_cast<TextInputBox *>(view); input && input->getSelected()) {
      input->endEditing();
      return true;
    }
    for (auto *child : view->getChildren()) if (self(self, child)) return true;
    return false;
  };
  for (auto *view : views) if (finishSelected(finishSelected, view)) break;
}

void SettingsScene::onPresentationOrientationChanged() {
  lastLayoutWidth = -1;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  ensureGameplaySkinSettingsController();
#endif
  syncPreviewPresentationConfiguration();
  ensureLayoutUpToDate();
}

void SettingsScene::onLanguageChanged() {
  View::LayoutBatchScope batch;
  Scene::onLanguageChanged();
  // Refresh presentation without committing input drafts or rebuilding views.
  refreshSettingsText(false);
  refreshAudioVideoControls(false);
  refreshInputMonitorText();
  updateDisplayPreviewUi();
}

void SettingsScene::update(float dt) {
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  updateGameplaySkinSettingsController();
  applyPendingSoundSetFolderPick();
#endif
  if (audioVideoSession != nullptr) {
    refreshAudioDiagnostics();
    const bool hadPreview = audioVideoSession->hasDisplayPreview();
    const auto previewResult =
        audioVideoSession->tick(std::chrono::steady_clock::now());
    if (previewResult.has_value() && !previewResult->message.empty()) {
      setDisplayStatus(previewResult->message,
                       previewResult->status == display::ApplyStatus::Applied
                           ? SDL_Color{157, 220, 176, 255}
                           : SDL_Color{255, 177, 170, 255});
    } else if (hadPreview && !audioVideoSession->hasDisplayPreview()) {
      setDisplayStatus(i18n::tr("settings.display_preview_ended_previous_settings_restored.message"),
                       {255, 209, 128, 255});
    }
    if (hadPreview && !audioVideoSession->hasDisplayPreview()) {
      displayDraft = context.settings.audioVideo.video;
    }
    updateDisplayPreviewUi();
  }
  if (previewActive) {
    ensurePreviewRenderer();
    ensurePreviewInputHandler();
    syncPreviewInputPlayAreaWidth();
    previewElapsedMicros +=
        static_cast<long long>(std::max(0.0f, dt) * 1000000.0f);
    if (previewElapsedMicros >= kPreviewLoopMicros) {
      resetPreviewSimulation();
    }
  }
  applyPendingDifficultyTableUpdates();
  applyPendingArchiveCacheCleanupStatus();
  applyPendingProfileArchiveCompletion();
  applyPendingProfileDocumentHandoff();
  refreshTablesIfLibraryChanged();
  updateInputSettingsState();
  if (activeTab == SettingsTab::Ir) {
    refreshIrSettingsPresentation();
  }
  ensureLayoutUpToDate();
}

void SettingsScene::renderScene() {
  if (rootLayout != nullptr) {
    rootLayout->setSize(rendering::window_width, rendering::window_height);
  }
  if (difficultyTableImportModalRoot != nullptr) {
    difficultyTableImportModalRoot->setSize(rendering::window_width,
                                            rendering::window_height);
  }
  if (inputConflictOverlayRoot != nullptr) {
    inputConflictOverlayRoot->setSize(rendering::window_width,
                                      rendering::window_height);
  }
  if (inputVirtualControllerEditorOverlayRoot != nullptr) {
    inputVirtualControllerEditorOverlayRoot->setSize(rendering::window_width,
                                                      rendering::window_height);
  }
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (gameplaySkinSafetyOverlayRoot != nullptr) {
    gameplaySkinSafetyOverlayRoot->setSize(rendering::window_width,
                                            rendering::window_height);
  }
  if (gameplaySkinBusyOverlayRoot != nullptr) {
    gameplaySkinBusyOverlayRoot->setSize(rendering::window_width,
                                          rendering::window_height);
  }
#endif
  if (previewActive && previewRenderer != nullptr) {
    syncPreviewPresentationConfiguration();
    capturePreviewVisualState();
    previewRenderer->refreshGeometry();
    RenderContext renderContext(context.uiBatchRenderer);
    RenderContext::UiBatchScope uiBatchScope(renderContext);
    previewRenderer->render(renderContext, previewElapsedMicros);
  }
}

EventHandleResult SettingsScene::handleEvents(SDL_Event &event) {
  const bool losesFocus = event.type == SDL_APP_WILLENTERBACKGROUND ||
                          event.type == SDL_APP_DIDENTERBACKGROUND ||
                          (event.type == SDL_WINDOWEVENT &&
                           (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                            event.window.event == SDL_WINDOWEVENT_MINIMIZED ||
                            event.window.event == SDL_WINDOWEVENT_HIDDEN));
  if (losesFocus && audioVideoSession != nullptr &&
      audioVideoSession->hasDisplayPreview()) {
    const auto result = audioVideoSession->onFocusLost();
    displayDraft = context.settings.audioVideo.video;
    setDisplayStatus(result.has_value() && !result->message.empty()
                         ? result->message
                         : i18n::tr("settings.display_preview_restored_after_focus_loss.message"),
                     {255, 209, 128, 255});
    updateDisplayPreviewUi();
  }
  for (auto *view : views) {
    if (!view->handleEvents(event)) {
      // Navigation callbacks can destroy this scene while consuming the event.
      return {};
    }
  }
  if (previewActive) {
    forwardPreviewInputEvent(event);
  }
  return {};
}

void SettingsScene::cleanupScene() {
  context.profileSwitchBlockers.scene = nullptr;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  if (gameplaySkinSettingsController != nullptr) {
    gameplaySkinSettingsController->close();
    gameplaySkinSettingsController.reset();
  }
  gameplaySkinSettingsProfileId.clear();
  gameplaySkinSettingsLayoutKey.clear();
  gameplaySkinUiMessage = {};
  gameplaySkinReplaceConfirmationArmed = false;
  gameplaySkinRemovalConfirmationKey.clear();
#endif
  stopProfileArchiveWork();
  if (audioVideoSession != nullptr) {
    const auto result = audioVideoSession->cleanup();
    if (!result.message.empty()) {
      SDL_Log("%s", result.message.resolve().c_str());
    }
    audioVideoSession.reset();
  }
  libraryTask.stopAndWait();
  archiveCacheMaintenance.stopAndWait();
  pendingDeleteChartEntryPath.clear();
  difficultyTableImportModalVisible = false;
  difficultyTableImportFinished = false;
  difficultyTableImportSucceeded = false;
  destroyPreviewInputHandler();
  destroyPreviewRenderer();
  inputProfileReplacementRegistration.reset();
  inputCaptureController.reset();
  inputCaptureAction.reset();
  inputGyroscopeAxisValue = 0.0F;
  inputGyroscopeSettingsError.clear();
  inputViewRebuildGate.reset();
  inputLastViewSignature.clear();
  profileInlineEditor.clear();
  previewLanePressed.clear();
  previewCombo = 0;
  previewScore = 0;
  rootLayout = nullptr;
  overlayPortal = nullptr;
  scrollView = nullptr;
  offsetInput = nullptr;
  summaryOffsetValueText = nullptr;
  visualOffsetInput = nullptr;
  summaryVisualOffsetValueText = nullptr;
  visibleTimeInput = nullptr;
  summaryVisibleTimeValueText = nullptr;
  summaryKeysoundValueText = nullptr;
  summaryBgaValueText = nullptr;
  summaryBgaBrightnessValueText = nullptr;
  summaryBgaBlurValueText = nullptr;
  summaryBgaDisplayValueText = nullptr;
  summaryLaneAngleValueText = nullptr;
  summaryLaneLengthValueText = nullptr;
  summaryLaneBeamLengthValueText = nullptr;
  summaryNoteStartPositionValueText = nullptr;
  summaryPreviewPlayAreaWidthValueText = nullptr;
  summaryNotePriorityValueText = nullptr;
  summaryJudgementIndicatorRangeValueText = nullptr;
  judgementIndicatorYInput = nullptr;
  judgementIndicatorWidthInput = nullptr;
  judgementIndicatorRangeInput = nullptr;
  visibleTimeModeText = nullptr;
  keysoundModeText = nullptr;
  prepMetronomeModeText = nullptr;
  startLaneIndicatorsModeText = nullptr;
  showInvisibleNotesModeText = nullptr;
  touchVisualizationModeText = nullptr;
  hispeedAutoAdjustModeText = nullptr;
  notePriorityModeText = nullptr;
  judgementIndicatorModeText = nullptr;
  judgementIndicatorRenderModeText = nullptr;
  bgaModeText = nullptr;
  bgaDisplayModeText = nullptr;
  archiveCacheCleanupButtonText = nullptr;
  archiveCacheCleanupStatusText = nullptr;
  profileTabText = nullptr;
  profileStatusText = nullptr;
  profileCreateNameInput = nullptr;
  visibleTimeModeButton = nullptr;
  keysoundModeButton = nullptr;
  prepMetronomeModeButton = nullptr;
  startLaneIndicatorsModeButton = nullptr;
  showInvisibleNotesModeButton = nullptr;
  touchVisualizationModeButton = nullptr;
  hispeedAutoAdjustModeButton = nullptr;
  notePriorityModeButton = nullptr;
  judgementIndicatorModeButton = nullptr;
  judgementIndicatorRenderModeButton = nullptr;
  bgaModeButton = nullptr;
  bgaDisplayModeButton = nullptr;
  archiveCacheCleanupButton = nullptr;
  profileTabButton = nullptr;
  timingTabButton = nullptr;
  visualTabButton = nullptr;
  laneTabButton = nullptr;
  inputTabButton = nullptr;
  miscTabButton = nullptr;
  audioTabButton = nullptr;
  displayTabButton = nullptr;
  difficultyTablesTabButton = nullptr;
  bmsLibraryTabButton = nullptr;
  irTabButton = nullptr;
  timingTabText = nullptr;
  visualTabText = nullptr;
  laneTabText = nullptr;
  inputTabText = nullptr;
  miscTabText = nullptr;
  audioTabText = nullptr;
  displayTabText = nullptr;
  difficultyTablesTabText = nullptr;
  bmsLibraryTabText = nullptr;
  irTabText = nullptr;
  irPendingCountText = nullptr;
  irAwaitingCountText = nullptr;
  irBlockedCountText = nullptr;
  irFailedCountText = nullptr;
  irStatusText = nullptr;
  irServerOriginInput = nullptr;
  irApiKeyInput = nullptr;
  irSettingsModel.reset();
  irPendingDiscardRowId.reset();
  irKeyEditorActive = false;
  irStatusIsError = false;
  irStatusMessage = {};
  bgaBrightnessInput = nullptr;
  bgaBlurInput = nullptr;
  laneAngleInput = nullptr;
  laneLengthInput = nullptr;
  laneBeamLengthInput = nullptr;
  noteStartPositionInput = nullptr;
  tableUrlInput = nullptr;
  difficultyTableStatusText = nullptr;
  chartFolderStatusText = nullptr;
  difficultyTableImportModalRoot = nullptr;
  difficultyTableImportProgressFill = nullptr;
  difficultyTableImportTitleText = nullptr;
  difficultyTableImportStatusText = nullptr;
  difficultyTableImportTableText = nullptr;
  difficultyTableImportProgressText = nullptr;
  difficultyTableImportCloseButton = nullptr;
  audioDeviceDropdown = nullptr;
  audioSampleRateDropdown = nullptr;
  audioBufferDropdown = nullptr;
  displayModeDropdown = nullptr;
  displayIndexDropdown = nullptr;
  displayResolutionDropdown = nullptr;
  displayVsyncDropdown = nullptr;
  displayFrameCapDropdown = nullptr;
  masterVolumeInput = nullptr;
  bgmVolumeInput = nullptr;
  keysoundVolumeInput = nullptr;
  audioEffectiveText = nullptr;
  audioStatusText = nullptr;
  displayStatusText = nullptr;
  displayPreviewOverlayRoot = nullptr;
  displayPreviewCountdownText = nullptr;
  displayPreviewStatusText = nullptr;
  displayPreviewKeepButton = nullptr;
  inputPlayerDropdown = nullptr;
  inputKeyModeDropdown = nullptr;
  inputDeviceDropdown = nullptr;
  inputMonitorText = nullptr;
  inputCaptureStateText = nullptr;
  inputErrorText = nullptr;
  inputConflictOverlayRoot = nullptr;
  inputVirtualControllerEditorOverlayRoot = nullptr;
#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
  gameplaySkinSafetyOverlayRoot = nullptr;
  gameplaySkinStatusText = nullptr;
  gameplaySkinUiMessageText = nullptr;
  gameplaySkinConfigurationDigestText = nullptr;
  gameplaySkinBusyOverlayRoot = nullptr;
  gameplaySkinBusyOverlayStatusText = nullptr;
  gameplaySkinBusyOverlayCancelButton = nullptr;
#endif
  lastLayoutWidth = -1;
  lastLayoutHeight = -1;
  lastSafeTop = -1;
  lastSafeLeft = -1;
  lastSafeBottom = -1;
  lastSafeRight = -1;
}
