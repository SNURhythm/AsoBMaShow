#include "SettingsSceneShared.h"
#include "../ArchiveFile.h"
#include "../view/ScrollView.h"
#include "play/BMSRenderer.h"

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

std::string formatCacheCleanupResult(
    const archive_file::TemporaryCacheCleanupResult &result) {
  if (!result.cacheExisted || result.removedEntries == 0) {
    return result.skippedEntries == 0
               ? "Temporary archive cache is already empty."
               : "Temporary archive cache only contains active files.";
  }
  std::string message = "Removed " + formatCacheBytes(result.removedBytes) +
                        " (" + std::to_string(result.removedEntries) +
                        " entries).";
  if (result.skippedEntries > 0) {
    message += " Skipped " + std::to_string(result.skippedEntries) +
               " active file";
    if (result.skippedEntries != 1) {
      message += "s";
    }
    message += ".";
  }
  return message;
}

std::string formatCacheUsageResult(
    const archive_file::TemporaryCacheUsageResult &result) {
  if (!result.cacheExisted || result.entries == 0) {
    return "Temporary archive cache is empty.";
  }
  return "Temporary archive cache uses " + formatCacheBytes(result.bytes) +
         " (" + std::to_string(result.entries) + " entries).";
}
} // namespace

void SettingsScene::requestArchiveCacheCleanupStatus(const std::string &text,
                                                     const SDL_Color &color) {
  std::lock_guard<std::mutex> lock(archiveCacheCleanupStatusMutex);
  pendingArchiveCacheCleanupStatus = true;
  pendingArchiveCacheCleanupStatusText = text;
  pendingArchiveCacheCleanupStatusColor = color;
}

void SettingsScene::applyPendingArchiveCacheCleanupStatus() {
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(archiveCacheCleanupStatusMutex);
    if (!pendingArchiveCacheCleanupStatus) {
      return;
    }
    archiveCacheCleanupStatusMessage = pendingArchiveCacheCleanupStatusText;
    archiveCacheCleanupStatusColor = pendingArchiveCacheCleanupStatusColor;
    pendingArchiveCacheCleanupStatus = false;
    changed = true;
  }

  if (archiveCacheCleanupStatusText != nullptr) {
    archiveCacheCleanupStatusText->setText(archiveCacheCleanupStatusMessage);
    archiveCacheCleanupStatusText->setColor(archiveCacheCleanupStatusColor);
  }
  if (archiveCacheCleanupButtonText != nullptr) {
    archiveCacheCleanupButtonText->setText(
        archiveCacheCleanupRunning.load() ? "Cleaning..." : "Clean Up");
  }
  if (changed && rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
  if (changed && scrollView != nullptr) {
    scrollView->refreshContentLayout();
  }
}

void SettingsScene::cleanupTemporaryArchiveCache() {
  bool expected = false;
  if (!archiveCacheCleanupRunning.compare_exchange_strong(expected, true)) {
    return;
  }

  const std::uint64_t generation =
      archiveCacheStatusGeneration.fetch_add(1, std::memory_order_relaxed) + 1;

  if (archiveCacheCleanupThread.joinable()) {
    archiveCacheCleanupThread.join();
  }

  archiveCacheCleanupStatusMessage = "Cleaning temporary archive cache...";
  archiveCacheCleanupStatusColor = {239, 244, 251, 255};
  if (archiveCacheCleanupStatusText != nullptr) {
    archiveCacheCleanupStatusText->setText(archiveCacheCleanupStatusMessage);
    archiveCacheCleanupStatusText->setColor(archiveCacheCleanupStatusColor);
  }
  if (archiveCacheCleanupButtonText != nullptr) {
    archiveCacheCleanupButtonText->setText("Cleaning...");
  }

  archiveCacheCleanupThread =
      std::jthread([this, generation](const std::stop_token &token) {
        archive_file::TemporaryCacheCleanupResult result;
        std::string errorMessage;
        const std::vector<std::filesystem::path> protectedPaths =
            context.jukebox.activeMaterializedVideoPaths();
        const bool cleaned =
            archive_file::cleanupTemporaryCache(result, protectedPaths,
                                                &errorMessage);

        if (token.stop_requested()) {
          archiveCacheCleanupRunning = false;
          return;
        }

        archiveCacheCleanupRunning = false;
        if (archiveCacheStatusGeneration.load(std::memory_order_relaxed) !=
            generation) {
          return;
        }
        if (!cleaned) {
          requestArchiveCacheCleanupStatus(
              errorMessage.empty() ? "Archive cache cleanup failed."
                                   : "Archive cache cleanup failed: " +
                                         errorMessage,
              {255, 177, 170, 255});
          return;
        }

        requestArchiveCacheCleanupStatus(formatCacheCleanupResult(result),
                                         {181, 228, 165, 255});
      });
}

void SettingsScene::measureTemporaryArchiveCache() {
  if (archiveCacheCleanupRunning.load(std::memory_order_relaxed)) {
    return;
  }

  bool expected = false;
  if (!archiveCacheMeasureRunning.compare_exchange_strong(expected, true)) {
    return;
  }

  const std::uint64_t generation =
      archiveCacheStatusGeneration.fetch_add(1, std::memory_order_relaxed) + 1;

  if (archiveCacheMeasureThread.joinable()) {
    archiveCacheMeasureThread.join();
  }

  archiveCacheCleanupStatusMessage = "Measuring temporary archive cache...";
  archiveCacheCleanupStatusColor = {239, 244, 251, 255};
  if (archiveCacheCleanupStatusText != nullptr) {
    archiveCacheCleanupStatusText->setText(archiveCacheCleanupStatusMessage);
    archiveCacheCleanupStatusText->setColor(archiveCacheCleanupStatusColor);
  }

  archiveCacheMeasureThread =
      std::jthread([this, generation](const std::stop_token &token) {
        archive_file::TemporaryCacheUsageResult result;
        std::string errorMessage;
        const bool measured =
            archive_file::measureTemporaryCache(result, &errorMessage, &token);

        if (token.stop_requested()) {
          archiveCacheMeasureRunning = false;
          return;
        }

        archiveCacheMeasureRunning = false;
        if (archiveCacheStatusGeneration.load(std::memory_order_relaxed) !=
            generation) {
          return;
        }
        if (!measured) {
          requestArchiveCacheCleanupStatus(
              errorMessage.empty() ? "Archive cache measurement failed."
                                   : "Archive cache measurement failed: " +
                                         errorMessage,
              {255, 177, 170, 255});
          return;
        }

        requestArchiveCacheCleanupStatus(formatCacheUsageResult(result),
                                         {157, 177, 200, 255});
      });
}

void SettingsScene::init() {
  lastLayoutWidth = -1;
  observedLibraryRevision = ChartDBHelper::GetInstance().GetLibraryRevision();
  ensureLayoutUpToDate();
}

void SettingsScene::update(float dt) {
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
  refreshTablesIfLibraryChanged();
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
  if (previewActive && previewRenderer != nullptr) {
    previewRenderer->setVisibleTimeGreenNumber(
        context.settings.visibleTimeGreenNumber);
    previewRenderer->setVisibleTimeBpmStrategy(
        context.settings.visibleTimeBpmStrategy);
    if (previewChart != nullptr) {
      previewRenderer->setPlayAreaWidth(
          context.settings.playAreaWidthForKeyMode(previewChart->Meta.KeyMode));
    }
    previewRenderer->setLaneBeamLengthPercent(
        context.settings.laneBeamLengthPercent);
    previewRenderer->setNoteStartPositionPercent(
        context.settings.noteStartPositionPercent);
    previewRenderer->setShowInvisibleNotes(context.settings.showInvisibleNotes);
    previewRenderer->setJudgementIndicatorConfig(
        context.settings.judgementIndicatorEnabled,
        context.settings.judgementIndicatorY,
        context.settings.judgementIndicatorWidthScale,
        context.settings.judgementIndicatorRenderMode ==
            AppSettings::JudgementIndicatorRenderMode::Hud2D);
    previewRenderer->setJudgementTextY(context.settings.judgementTextY);
    previewRenderer->setJudgementTimingFastSlowCriteria(
        context.settings.judgementTimingFastSlowCriteria);
    previewRenderer->setJudgementTimingMillisecondsCriteria(
        context.settings.judgementTimingMillisecondsCriteria);
    previewRenderer->setJudgementCounterEnabled(
        context.settings.judgementCounterEnabled);
    previewRenderer->setJudgementCounterPosition(
        context.settings.judgementCounterPosition);
    previewRenderer->setGaugeBarPosition(context.settings.gaugeBarPosition);
    previewRenderer->refreshGeometry();
    RenderContext renderContext;
    previewRenderer->render(renderContext, previewElapsedMicros);
  }
}

EventHandleResult SettingsScene::handleEvents(SDL_Event &event) {
  bool handledByView = false;
  for (auto *view : views) {
    if (!view->handleEvents(event)) {
      handledByView = true;
      break;
    }
  }
  if (previewActive && !handledByView) {
    forwardPreviewInputEvent(event);
  }
  return {};
}

void SettingsScene::cleanupScene() {
  if (difficultyTableJobThread.joinable()) {
    SDL_Log("Joining difficultyTableJobThread");
    difficultyTableJobThread.request_stop();
    difficultyTableJobThread.join();
  }
  if (archiveCacheCleanupThread.joinable()) {
    SDL_Log("Joining archiveCacheCleanupThread");
    archiveCacheCleanupThread.request_stop();
    archiveCacheCleanupThread.join();
  }
  if (archiveCacheMeasureThread.joinable()) {
    SDL_Log("Joining archiveCacheMeasureThread");
    archiveCacheMeasureThread.request_stop();
    archiveCacheMeasureThread.join();
  }
  pendingDeleteChartEntryPath.clear();
  difficultyTableImportModalVisible = false;
  difficultyTableImportFinished = false;
  difficultyTableImportSucceeded = false;
  destroyPreviewInputHandler();
  destroyPreviewRenderer();
  previewLanePressed.clear();
  previewCombo = 0;
  previewScore = 0;
  rootLayout = nullptr;
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
  judgementIndicatorYInput = nullptr;
  judgementIndicatorWidthInput = nullptr;
  visibleTimeModeText = nullptr;
  visibleTimeBpmStrategyText = nullptr;
  keysoundModeText = nullptr;
  notePriorityModeText = nullptr;
  judgementIndicatorModeText = nullptr;
  judgementIndicatorRenderModeText = nullptr;
  bgaModeText = nullptr;
  bgaDisplayModeText = nullptr;
  archiveCacheCleanupButtonText = nullptr;
  archiveCacheCleanupStatusText = nullptr;
  visibleTimeModeButton = nullptr;
  visibleTimeBpmStrategyButton = nullptr;
  keysoundModeButton = nullptr;
  notePriorityModeButton = nullptr;
  judgementIndicatorModeButton = nullptr;
  judgementIndicatorRenderModeButton = nullptr;
  bgaModeButton = nullptr;
  bgaDisplayModeButton = nullptr;
  archiveCacheCleanupButton = nullptr;
  timingTabButton = nullptr;
  visualTabButton = nullptr;
  laneTabButton = nullptr;
  miscTabButton = nullptr;
  difficultyTablesTabButton = nullptr;
  bmsLibraryTabButton = nullptr;
  timingTabText = nullptr;
  visualTabText = nullptr;
  laneTabText = nullptr;
  miscTabText = nullptr;
  difficultyTablesTabText = nullptr;
  bmsLibraryTabText = nullptr;
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
  lastLayoutWidth = -1;
  lastLayoutHeight = -1;
  lastSafeTop = -1;
  lastSafeLeft = -1;
  lastSafeBottom = -1;
  lastSafeRight = -1;
}
