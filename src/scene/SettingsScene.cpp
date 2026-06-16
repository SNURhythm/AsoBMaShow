#include "SettingsSceneShared.h"
#include "play/BMSRenderer.h"

using namespace settings_scene;

void SettingsScene::init() { ensureLayoutUpToDate(); }

void SettingsScene::update(float dt) {
  if (previewActive) {
    ensurePreviewRenderer();
    previewElapsedMicros +=
        static_cast<long long>(std::max(0.0f, dt) * 1000000.0f);
    if (previewElapsedMicros >= kPreviewLoopMicros) {
      resetPreviewSimulation();
    }
  }
  applyPendingDifficultyTableUpdates();
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
    previewRenderer->setLaneBeamLengthPercent(
        context.settings.laneBeamLengthPercent);
    previewRenderer->setShowInvisibleNotes(context.settings.showInvisibleNotes);
    previewRenderer->setJudgementIndicatorConfig(
        context.settings.judgementIndicatorEnabled,
        context.settings.judgementIndicatorY,
        context.settings.judgementIndicatorWidthScale,
        context.settings.judgementIndicatorRenderMode ==
            AppSettings::JudgementIndicatorRenderMode::Hud2D);
    previewRenderer->refreshGeometry();
    RenderContext renderContext;
    previewRenderer->render(renderContext, previewElapsedMicros);
  }
}

void SettingsScene::cleanupScene() {
  if (difficultyTableJobThread.joinable()) {
    SDL_Log("Joining difficultyTableJobThread");
    difficultyTableJobThread.request_stop();
    difficultyTableJobThread.join();
  }
  pendingDeleteChartEntryPath.clear();
  difficultyTableImportModalVisible = false;
  difficultyTableImportFinished = false;
  difficultyTableImportSucceeded = false;
  destroyPreviewRenderer();
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
  visibleTimeModeButton = nullptr;
  visibleTimeBpmStrategyButton = nullptr;
  keysoundModeButton = nullptr;
  notePriorityModeButton = nullptr;
  judgementIndicatorModeButton = nullptr;
  judgementIndicatorRenderModeButton = nullptr;
  bgaModeButton = nullptr;
  bgaDisplayModeButton = nullptr;
  timingTabButton = nullptr;
  visualTabButton = nullptr;
  laneTabButton = nullptr;
  tablesTabButton = nullptr;
  bgaBrightnessInput = nullptr;
  bgaBlurInput = nullptr;
  laneAngleInput = nullptr;
  laneLengthInput = nullptr;
  laneBeamLengthInput = nullptr;
  tableUrlInput = nullptr;
  difficultyTableStatusText = nullptr;
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
