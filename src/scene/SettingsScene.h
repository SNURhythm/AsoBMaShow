#pragma once

#include "../ChartDBHelper.h"
#include "Scene.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class View;
class TextView;
class TextInputBox;
class Button;
class ScrollView;
class BMSRenderer;

namespace bms_parser {
class Chart;
}

class SettingsScene : public Scene {
public:
  explicit SettingsScene(ApplicationContext &context) : Scene(context) {}

  void init() override;
  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  enum class SettingsTab {
    Timing,
    Visual,
    Lane,
    Tables,
  };

  View *rootLayout = nullptr;
  TextInputBox *offsetInput = nullptr;
  TextView *summaryOffsetValueText = nullptr;
  TextInputBox *visualOffsetInput = nullptr;
  TextView *summaryVisualOffsetValueText = nullptr;
  TextInputBox *visibleTimeInput = nullptr;
  TextView *summaryVisibleTimeValueText = nullptr;
  TextView *summaryKeysoundValueText = nullptr;
  TextView *summaryBgaValueText = nullptr;
  TextView *summaryBgaBrightnessValueText = nullptr;
  TextView *summaryBgaBlurValueText = nullptr;
  TextView *summaryBgaDisplayValueText = nullptr;
  TextView *summaryLaneAngleValueText = nullptr;
  TextView *summaryLaneLengthValueText = nullptr;
  TextView *summaryNotePriorityValueText = nullptr;
  TextInputBox *judgementIndicatorYInput = nullptr;
  TextView *visibleTimeModeText = nullptr;
  TextView *keysoundModeText = nullptr;
  TextView *notePriorityModeText = nullptr;
  TextView *judgementIndicatorModeText = nullptr;
  TextView *judgementIndicatorRenderModeText = nullptr;
  TextView *bgaModeText = nullptr;
  TextView *bgaDisplayModeText = nullptr;
  Button *visibleTimeModeButton = nullptr;
  Button *keysoundModeButton = nullptr;
  Button *notePriorityModeButton = nullptr;
  Button *judgementIndicatorModeButton = nullptr;
  Button *judgementIndicatorRenderModeButton = nullptr;
  Button *bgaModeButton = nullptr;
  Button *bgaDisplayModeButton = nullptr;
  Button *timingTabButton = nullptr;
  Button *visualTabButton = nullptr;
  Button *laneTabButton = nullptr;
  Button *tablesTabButton = nullptr;
  TextInputBox *bgaBrightnessInput = nullptr;
  TextInputBox *bgaBlurInput = nullptr;
  TextInputBox *laneAngleInput = nullptr;
  TextInputBox *laneLengthInput = nullptr;
  TextInputBox *tableUrlInput = nullptr;
  TextView *difficultyTableStatusText = nullptr;
  ScrollView *scrollView = nullptr;
  bool previewActive = false;
  bool previewPanelFolded = false;
  bms_parser::Chart *previewChart = nullptr;
  BMSRenderer *previewRenderer = nullptr;
  long long previewElapsedMicros = 0;
  SettingsTab activeTab = SettingsTab::Timing;
  std::vector<DifficultyTableInfo> difficultyTables;
  std::jthread difficultyTableJobThread;
  std::atomic_bool difficultyTableJobRunning = false;
  std::mutex difficultyTableStatusMutex;
  bool pendingDifficultyTableStatus = false;
  bool pendingDifficultyTableReload = false;
  std::string pendingDifficultyTableStatusText;
  SDL_Color pendingDifficultyTableStatusColor{157, 177, 200, 255};
  std::string difficultyTableStatusMessage;
  SDL_Color difficultyTableStatusColor{157, 177, 200, 255};
  std::string tableUrlText;
  int pendingDeleteDifficultyTableId = 0;
  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
  int lastSafeTop = -1;
  int lastSafeLeft = -1;
  int lastSafeBottom = -1;
  int lastSafeRight = -1;

  void initView();
  void resetViewState();
  void ensureLayoutUpToDate();
  void startLanePreview();
  void stopLanePreview();
  void ensurePreviewRenderer();
  void destroyPreviewRenderer();
  void resetPreviewSimulation();
  void loadDifficultyTables();
  void requestDifficultyTableStatus(const std::string &text,
                                    const SDL_Color &color,
                                    bool reloadTables = false);
  void applyPendingDifficultyTableUpdates();
  void addDifficultyTableFromUrl();
  void updateDifficultyTableFromSource(int tableId);
  void deleteDifficultyTable(int tableId);
  void refreshSettingsText();
  void persistSettings();
  void syncOffsetInputText(bool force = false);
  void syncVisualOffsetInputText(bool force = false);
  void syncVisibleTimeInputText(bool force = false);
  void syncBgaBrightnessInputText(bool force = false);
  void syncBgaBlurInputText(bool force = false);
  void syncLaneAngleInputText(bool force = false);
  void syncLaneLengthInputText(bool force = false);
  void syncJudgementIndicatorYInputText(bool force = false);
  void commitOffsetInput();
  void commitVisualOffsetInput();
  void commitVisibleTimeInput();
  void commitBgaBrightnessInput();
  void commitBgaBlurInput();
  void commitLaneAngleInput();
  void commitLaneLengthInput();
  void commitJudgementIndicatorYInput();
};
