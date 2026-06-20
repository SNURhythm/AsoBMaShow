#pragma once

#include "../ChartDBHelper.h"
#include "Scene.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class View;
class TextView;
class TextInputBox;
class Button;
class ScrollView;
class BMSRenderer;
class RhythmInputHandler;
class RhythmLaneInputController;

namespace settings_scene {
struct LayoutMetrics;
}

namespace bms_parser {
class Chart;
class Note;
}

#include "../input/IRhythmControl.h"

class SettingsScene : public Scene, public IRhythmControl {
public:
  explicit SettingsScene(ApplicationContext &context) : Scene(context) {}

  void init() override;
  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;
  EventHandleResult handleEvents(SDL_Event &event) override;
  bms_parser::Note *pressLane(int lane, double inputDelay = 0) override;
  bms_parser::Note *pressLane(int mainLane, int compensateLane,
                              double inputDelay = 0) override;
  bms_parser::Note *releaseLane(int lane, double inputDelay = 0) override;

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
  TextView *summaryLaneBeamLengthValueText = nullptr;
  TextView *summaryNoteStartPositionValueText = nullptr;
  TextView *summaryPreviewPlayAreaWidthValueText = nullptr;
  TextView *summaryNotePriorityValueText = nullptr;
  TextInputBox *judgementIndicatorYInput = nullptr;
  TextInputBox *judgementIndicatorWidthInput = nullptr;
  TextView *visibleTimeModeText = nullptr;
  TextView *visibleTimeBpmStrategyText = nullptr;
  TextView *keysoundModeText = nullptr;
  TextView *showInvisibleNotesModeText = nullptr;
  TextView *notePriorityModeText = nullptr;
  TextView *judgementIndicatorModeText = nullptr;
  TextView *judgementIndicatorRenderModeText = nullptr;
  TextView *bgaModeText = nullptr;
  TextView *bgaDisplayModeText = nullptr;
  Button *visibleTimeModeButton = nullptr;
  Button *visibleTimeBpmStrategyButton = nullptr;
  Button *keysoundModeButton = nullptr;
  Button *showInvisibleNotesModeButton = nullptr;
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
  TextInputBox *laneBeamLengthInput = nullptr;
  TextInputBox *noteStartPositionInput = nullptr;
  TextInputBox *tableUrlInput = nullptr;
  TextView *difficultyTableStatusText = nullptr;
  View *difficultyTableImportModalRoot = nullptr;
  View *difficultyTableImportProgressFill = nullptr;
  TextView *difficultyTableImportTitleText = nullptr;
  TextView *difficultyTableImportStatusText = nullptr;
  TextView *difficultyTableImportTableText = nullptr;
  TextView *difficultyTableImportProgressText = nullptr;
  Button *difficultyTableImportCloseButton = nullptr;
  ScrollView *scrollView = nullptr;
  bool previewActive = false;
  bool previewPanelFolded = false;
  int previewPanelPage = 0;
  bms_parser::Chart *previewChart = nullptr;
  BMSRenderer *previewRenderer = nullptr;
  RhythmInputHandler *previewInputHandler = nullptr;
  RhythmLaneInputController *previewLaneController = nullptr;
  std::unordered_map<int, bool> previewLanePressed;
  long long previewElapsedMicros = 0;
  int previewCombo = 0;
  int previewScore = 0;
  SettingsTab activeTab = SettingsTab::Timing;
  std::vector<DifficultyTableInfo> difficultyTables;
  std::vector<ChartEntry> chartEntries;
  std::jthread difficultyTableJobThread;
  std::atomic_bool difficultyTableJobRunning = false;
  std::mutex difficultyTableStatusMutex;
  bool pendingDifficultyTableStatus = false;
  bool pendingDifficultyTableReload = false;
  bool pendingDifficultyTableImportProgress = false;
  bool pendingDifficultyTableImportFinished = false;
  bool pendingDifficultyTableImportSucceeded = false;
  int pendingDifficultyTableImportCurrent = 0;
  int pendingDifficultyTableImportTotal = 0;
  std::string pendingDifficultyTableImportName;
  std::string pendingDifficultyTableImportStatusText;
  std::string pendingDifficultyTableStatusText;
  SDL_Color pendingDifficultyTableStatusColor{157, 177, 200, 255};
  std::string difficultyTableStatusMessage;
  SDL_Color difficultyTableStatusColor{157, 177, 200, 255};
  bool difficultyTableImportModalVisible = false;
  bool difficultyTableImportFinished = false;
  bool difficultyTableImportSucceeded = false;
  int difficultyTableImportCurrent = 0;
  int difficultyTableImportTotal = 0;
  std::string difficultyTableImportName;
  std::string difficultyTableImportStatusMessage;
  std::string tableUrlText;
  std::unordered_map<std::string, bool> chartEntryICloudBackupExcluded;
  int pendingDeleteDifficultyTableId = 0;
  std::string pendingDeleteChartEntryPath;
  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
  int lastSafeTop = -1;
  int lastSafeLeft = -1;
  int lastSafeBottom = -1;
  int lastSafeRight = -1;

  void initView();
  void resetViewState();
  void ensureLayoutUpToDate();
  View *buildVisibleTimeControls(const settings_scene::LayoutMetrics &metrics,
                                 bool includeDescription,
                                 bool compactAdjustments);
  void buildPreviewLayout(const settings_scene::LayoutMetrics &metrics);
  View *buildTimingTab(const settings_scene::LayoutMetrics &metrics);
  View *buildVisualTab(const settings_scene::LayoutMetrics &metrics);
  View *buildLaneTab(const settings_scene::LayoutMetrics &metrics);
  View *buildTablesTab(const settings_scene::LayoutMetrics &metrics);
  void
  buildDifficultyTableImportModal(const settings_scene::LayoutMetrics &metrics);
  void startLanePreview();
  void stopLanePreview();
  void ensurePreviewRenderer();
  void destroyPreviewRenderer();
  void ensurePreviewInputHandler();
  void destroyPreviewInputHandler();
  void forwardPreviewInputEvent(SDL_Event &event);
  void syncPreviewInputPlayAreaWidth();
  void resetPreviewSimulation();
  void loadDifficultyTables();
  void loadChartEntries();
  void requestDifficultyTableStatus(const std::string &text,
                                    const SDL_Color &color,
                                    bool reloadTables = false);
  void requestDifficultyTableImportProgress(int current, int total,
                                            const std::string &tableName,
                                            const std::string &statusText,
                                            bool finished,
                                            bool succeeded = false);
  void applyPendingDifficultyTableUpdates();
  void refreshDifficultyTableImportModal();
  void hideDifficultyTableImportModal();
  void addDifficultyTableFromUrl();
  void updateDifficultyTableFromSource(int tableId);
  void deleteDifficultyTable(int tableId);
  void deleteChartEntry(const std::string &entryPathText);
  void refreshChartLibrary();
  void refreshChartEntryBackupStatuses();
  void toggleChartEntryICloudBackup(const std::string &entryPathText);
  void refreshSettingsText();
  void persistSettings();
  void syncOffsetInputText(bool force = false);
  void syncVisualOffsetInputText(bool force = false);
  void syncVisibleTimeInputText(bool force = false);
  void syncBgaBrightnessInputText(bool force = false);
  void syncBgaBlurInputText(bool force = false);
  void syncLaneAngleInputText(bool force = false);
  void syncLaneLengthInputText(bool force = false);
  void syncLaneBeamLengthInputText(bool force = false);
  void syncNoteStartPositionInputText(bool force = false);
  void syncJudgementIndicatorYInputText(bool force = false);
  void syncJudgementIndicatorWidthInputText(bool force = false);
  void commitOffsetInput();
  void commitVisualOffsetInput();
  void commitVisibleTimeInput();
  void commitBgaBrightnessInput();
  void commitBgaBlurInput();
  void commitLaneAngleInput();
  void commitLaneLengthInput();
  void commitLaneBeamLengthInput();
  void commitNoteStartPositionInput();
  void commitJudgementIndicatorYInput();
  void commitJudgementIndicatorWidthInput();
};
