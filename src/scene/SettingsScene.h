#pragma once

#include "../ChartDBHelper.h"
#include "../ThreadCompat.h"
#include "Scene.h"
#include "play/Judge.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
class DropdownView;
class InputCaptureController;

namespace settings_scene {
struct LayoutMetrics;
}

namespace bms_parser {
class Chart;
class Note;
} // namespace bms_parser

#include "../input/IRhythmControl.h"
#include "../input/InputTypes.h"

class SettingsScene : public Scene, public IRhythmControl {
public:
  explicit SettingsScene(ApplicationContext &context);
  ~SettingsScene() override;

  void init() override;
  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;
  EventHandleResult handleEvents(SDL_Event &event) override;
  bms_parser::Note *pressLane(int lane, double inputDelay = 0) override;
  bms_parser::Note *pressLane(int mainLane, int compensateLane,
                              double inputDelay = 0) override;
  bms_parser::Note *releaseLane(int lane, double inputDelay = 0,
                                bool isBackSpin = false) override;

private:
  enum class SettingsTab {
    Timing,
    Visual,
    Lane,
    Input,
    Misc,
    DifficultyTables,
    BmsLibrary,
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
  TextView *summaryJudgementTextYValueText = nullptr;
  TextView *summaryJudgementIndicatorYValueText = nullptr;
  TextView *summaryJudgementIndicatorWidthValueText = nullptr;
  TextView *summaryJudgementCounterPositionValueText = nullptr;
  TextView *summaryJudgementTimingFastSlowValueText = nullptr;
  TextView *summaryJudgementTimingMillisecondsValueText = nullptr;
  TextView *summaryGaugeBarPositionValueText = nullptr;
  TextView *summaryNotePriorityValueText = nullptr;
  TextView *summaryUiThemeValueText = nullptr;
  TextInputBox *judgementIndicatorYInput = nullptr;
  TextInputBox *judgementIndicatorWidthInput = nullptr;
  TextView *visibleTimeModeText = nullptr;
  TextView *visibleTimeBpmStrategyText = nullptr;
  TextView *keysoundModeText = nullptr;
  TextView *prepMetronomeModeText = nullptr;
  TextView *showInvisibleNotesModeText = nullptr;
  TextView *touchVisualizationModeText = nullptr;
  TextView *floatingLaneCoverModeText = nullptr;
  TextView *archiveChartPreviewModeText = nullptr;
  TextView *notePriorityModeText = nullptr;
  TextView *judgementIndicatorModeText = nullptr;
  TextView *judgementIndicatorRenderModeText = nullptr;
  TextView *judgementTimingFastSlowCriteriaText = nullptr;
  TextView *judgementTimingMillisecondsCriteriaText = nullptr;
  TextView *judgementCounterModeText = nullptr;
  TextView *judgementCounterPositionText = nullptr;
  TextView *gaugeBarPositionText = nullptr;
  TextView *bgaModeText = nullptr;
  TextView *bgaDisplayModeText = nullptr;
  TextView *uiThemeModeText = nullptr;
  TextView *archiveCacheCleanupButtonText = nullptr;
  TextView *archiveCacheCleanupStatusText = nullptr;
  Button *visibleTimeModeButton = nullptr;
  Button *visibleTimeBpmStrategyButton = nullptr;
  Button *keysoundModeButton = nullptr;
  Button *prepMetronomeModeButton = nullptr;
  Button *showInvisibleNotesModeButton = nullptr;
  Button *touchVisualizationModeButton = nullptr;
  Button *floatingLaneCoverModeButton = nullptr;
  Button *archiveChartPreviewModeButton = nullptr;
  Button *notePriorityModeButton = nullptr;
  Button *judgementIndicatorModeButton = nullptr;
  Button *judgementIndicatorRenderModeButton = nullptr;
  Button *judgementTimingFastSlowCriteriaButton = nullptr;
  Button *judgementTimingMillisecondsCriteriaButton = nullptr;
  Button *judgementCounterModeButton = nullptr;
  Button *judgementCounterPositionButton = nullptr;
  Button *gaugeBarPositionButton = nullptr;
  Button *bgaModeButton = nullptr;
  Button *bgaDisplayModeButton = nullptr;
  Button *uiThemeModeButton = nullptr;
  Button *archiveCacheCleanupButton = nullptr;
  Button *timingTabButton = nullptr;
  Button *visualTabButton = nullptr;
  Button *laneTabButton = nullptr;
  Button *inputTabButton = nullptr;
  Button *miscTabButton = nullptr;
  Button *difficultyTablesTabButton = nullptr;
  Button *bmsLibraryTabButton = nullptr;
  TextView *timingTabText = nullptr;
  TextView *visualTabText = nullptr;
  TextView *laneTabText = nullptr;
  TextView *inputTabText = nullptr;
  TextView *miscTabText = nullptr;
  TextView *difficultyTablesTabText = nullptr;
  TextView *bmsLibraryTabText = nullptr;
  TextInputBox *bgaBrightnessInput = nullptr;
  TextInputBox *bgaBlurInput = nullptr;
  TextInputBox *laneAngleInput = nullptr;
  TextInputBox *laneLengthInput = nullptr;
  TextInputBox *laneBeamLengthInput = nullptr;
  TextInputBox *noteStartPositionInput = nullptr;
  TextInputBox *tableUrlInput = nullptr;
  TextView *difficultyTableStatusText = nullptr;
  TextView *chartFolderStatusText = nullptr;
  View *difficultyTableImportModalRoot = nullptr;
  View *difficultyTableImportProgressFill = nullptr;
  TextView *difficultyTableImportTitleText = nullptr;
  TextView *difficultyTableImportStatusText = nullptr;
  TextView *difficultyTableImportTableText = nullptr;
  TextView *difficultyTableImportProgressText = nullptr;
  Button *difficultyTableImportCloseButton = nullptr;
  ScrollView *scrollView = nullptr;
  DropdownView *inputPlayerDropdown = nullptr;
  DropdownView *inputKeyModeDropdown = nullptr;
  DropdownView *inputDeviceDropdown = nullptr;
  TextView *inputMonitorText = nullptr;
  TextView *inputCaptureStateText = nullptr;
  TextView *inputErrorText = nullptr;
  std::unique_ptr<InputCaptureController> inputCaptureController;
  int inputSelectedPlayer = 1;
  int inputSelectedKeyMode = 7;
  std::string inputSelectedDeviceId;
  bool inputPlayerDropdownOpen = false;
  bool inputKeyModeDropdownOpen = false;
  bool inputDeviceDropdownOpen = false;
  std::optional<input::LogicalAction> inputCaptureAction;
  std::string inputLastViewSignature;
  bool previewActive = false;
  bool previewPanelFolded = false;
  int previewPanelPage = 0;
  std::unique_ptr<bms_parser::Chart> previewChart;
  std::unique_ptr<BMSRenderer> previewRenderer;
  std::unique_ptr<RhythmInputHandler> previewInputHandler;
  std::unique_ptr<RhythmLaneInputController> previewLaneController;
  std::unordered_map<int, bool> previewLanePressed;
  long long previewElapsedMicros = 0;
  int previewCombo = 0;
  int previewScore = 0;
  int previewComboBreak = 0;
  std::map<Judgement, int> previewJudgeCount;
  SettingsTab activeTab = SettingsTab::Timing;
  std::vector<DifficultyTableInfo> difficultyTables;
  std::vector<ChartEntry> chartEntries;
  std::jthread difficultyTableJobThread;
  std::jthread archiveCacheCleanupThread;
  std::jthread archiveCacheMeasureThread;
  std::atomic_bool difficultyTableJobRunning = false;
  std::atomic_bool archiveCacheCleanupRunning = false;
  std::atomic_bool archiveCacheMeasureRunning = false;
  std::atomic<std::uint64_t> archiveCacheStatusGeneration = 0;
  std::mutex difficultyTableStatusMutex;
  std::mutex archiveCacheCleanupStatusMutex;
  bool pendingDifficultyTableStatus = false;
  bool pendingChartFolderStatus = false;
  bool pendingDifficultyTableReload = false;
  bool pendingDifficultyTableImportProgress = false;
  bool pendingDifficultyTableImportFinished = false;
  bool pendingDifficultyTableImportSucceeded = false;
  bool pendingArchiveCacheCleanupStatus = false;
  int pendingDifficultyTableImportCurrent = 0;
  int pendingDifficultyTableImportTotal = 0;
  std::string pendingDifficultyTableImportName;
  std::string pendingDifficultyTableImportStatusText;
  std::string pendingDifficultyTableStatusText;
  std::string pendingChartFolderStatusText;
  std::string pendingArchiveCacheCleanupStatusText;
  SDL_Color pendingDifficultyTableStatusColor{157, 177, 200, 255};
  SDL_Color pendingChartFolderStatusColor{157, 177, 200, 255};
  SDL_Color pendingArchiveCacheCleanupStatusColor{157, 177, 200, 255};
  std::string difficultyTableStatusMessage;
  SDL_Color difficultyTableStatusColor{157, 177, 200, 255};
  std::string chartFolderStatusMessage;
  SDL_Color chartFolderStatusColor{157, 177, 200, 255};
  std::string archiveCacheCleanupStatusMessage =
      "Temporary archive cache has not been cleaned yet.";
  SDL_Color archiveCacheCleanupStatusColor{157, 177, 200, 255};
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
  SettingsTab lastLaidOutTab = SettingsTab::Timing;
  std::uint64_t observedLibraryRevision = 0;

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
  View *buildInputTab(const settings_scene::LayoutMetrics &metrics);
  View *buildMiscTab(const settings_scene::LayoutMetrics &metrics);
  View *
  buildDifficultyTablesTab(const settings_scene::LayoutMetrics &metrics);
  View *buildBmsLibraryTab(const settings_scene::LayoutMetrics &metrics);
  void
  buildDifficultyTableImportModal(const settings_scene::LayoutMetrics &metrics);
  void startLanePreview();
  void stopLanePreview();
  void ensurePreviewRenderer();
  void destroyPreviewRenderer();
  void ensurePreviewInputHandler();
  void destroyPreviewInputHandler();
  void ensureInputCaptureController();
  void updateInputSettingsState();
  void refreshInputMonitorText();
  void refreshInputDropdowns();
  void requestInputViewRebuild();
  std::string inputViewSignature() const;
  void forwardPreviewInputEvent(SDL_Event &event);
  void syncPreviewInputPlayAreaWidth();
  void resetPreviewHudSample();
  void publishPreviewJudgement(const JudgeResult &judgeResult);
  void resetPreviewSimulation();
  void loadDifficultyTables();
  void loadChartEntries();
  void requestDifficultyTableStatus(const std::string &text,
                                    const SDL_Color &color,
                                    bool reloadTables = false);
  void requestChartFolderStatus(const std::string &text,
                                const SDL_Color &color,
                                bool reloadTables = false);
  void requestDifficultyTableImportProgress(int current, int total,
                                            const std::string &tableName,
                                            const std::string &statusText,
                                            bool finished,
                                            bool succeeded = false);
  void requestArchiveCacheCleanupStatus(const std::string &text,
                                        const SDL_Color &color);
  void applyPendingDifficultyTableUpdates();
  void applyPendingArchiveCacheCleanupStatus();
  void refreshTablesIfLibraryChanged();
  void refreshDifficultyTableImportModal();
  void hideDifficultyTableImportModal();
  void addDifficultyTableFromUrl();
  void updateDifficultyTableFromSource(int tableId);
  void deleteDifficultyTable(int tableId);
  void deleteChartEntry(const std::string &entryPathText);
  void refreshChartLibrary();
  void refreshChartEntryBackupStatuses();
  void toggleChartEntryICloudBackup(const std::string &entryPathText);
  void measureTemporaryArchiveCache();
  void cleanupTemporaryArchiveCache();
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
