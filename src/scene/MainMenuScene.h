#pragma once
#include "../BmsSearchService.h"
#include "../ChartLibraryScanner.h"
#include "../library/ChartLibraryPlatform.h"
#include "../library/ChartLibraryTaskService.h"
#include "../view/RecyclerView.h"
#include "ChartFilterSortPanelView.h"
#include "Scene.h"
#include "../repositories/ChartRepository.h"
#include "../repositories/ReplayRepository.h"
#include "../ReplayRecordFilters.h"
#include "../ResultRecordSummary.h"
#include "../ReplayVideoExporter.h"
#include "../PlatformDocumentHandoff.h"
#include "../replay/ReplayFileActionSelection.h"
#include "../repositories/ScoreRepository.h"
#include "../ir/IrRankingModal.h"
#include "../ThreadCompat.h"
#include "../path.h"
#include "../utils/Debouncer.h"
#include "../view/ImageView.h"
#include "../view/ContextMenuView.h"
#include "../view/ResultRecordListView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"
#include <filesystem>
#include <deque>
#include <functional>
#include <thread>
#include <unordered_set>
#include <vector>
#include "../targets.h"
#include "../audio/Jukebox.h"
#include "../video/VideoPlayer.h"
#include "MainMenuLibrary.h"
#include "MainMenuPlayOptionsModal.h"
#include "MainMenuProfileSelections.h"
#include "ReplayRecordsModal.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

class Button;
class DropdownView;
class OverlayPortal;
class BlockingOverlayView;
class DecideLoadingOverlay;
class ChartPreloadWorker;
class PlayOptionsPanelView;
class ScrollView;
struct CoursePlaySession;
struct StartOptions;
class View;

struct MainMenuParseLogRow {
  std::uint64_t id = 0;
  std::string text;
};

class MainMenuScene : public Scene {
public:
  inline explicit MainMenuScene(ApplicationContext &context) : Scene(context) {}
  void init() override;
  void onPause() override;
  void onResume() override;
  EventHandleResult handleEvents(SDL_Event &event) override;

  void update(float dt) override;
  void renderScene() override;
  void cleanupScene() override;

private:
  std::optional<ChartRepository::Session> chartSession;
  std::atomic_bool willStart = false;
  std::unique_ptr<bms_parser::Chart> selectedChart;
  mutable std::mutex selectedChartMutex;
  std::atomic_bool selectedChartMediaReady = false;
  std::atomic_bool selectedChartReusableForStart = false;

  std::thread loadThread;
  // Preview chart loading runs on the shared ChartPreloadWorker (single
  // scene-lifetime thread, debounced and latest-wins) so a selection change
  // never spawns or joins a per-selection thread on the UI thread.
  ChartPreloadWorker *previewWorker_ = nullptr;
  std::mutex previewJukeboxLoadMutex;
  bool pendingStopAndClearSelectedChartAfterPreview = false;
  std::jthread findBmsThread;
  std::jthread replayLoadThread;
  std::shared_ptr<std::atomic_bool> replayLoadCancelToken =
      std::make_shared<std::atomic_bool>(false);
  std::mutex replayLoadCompletionMutex;
  std::function<void()> pendingReplayLoadCompletion;
  std::atomic_bool replayLoadInProgress = false;
  std::jthread replayExportThread;
  std::jthread unzipThread;
  bool prioritizeVisibleArtworkBindings = false;
  std::atomic_bool replayExportInProgress = false;
  bool replayResultRecallInProgress = false;
  bool replayIrUploadInProgress = false;
  std::unordered_map<std::string, std::uint64_t> replayIrObservedRevisions;
  std::atomic_bool unzipInProgress = false;
  std::atomic_bool tasksModalOpenRequested = false;
  using LibraryTaskStatus = chart_library_tasks::TaskStatus;
  using LibraryTaskInfo = chart_library_tasks::TaskInfo;
  using LibraryTaskProgressSnapshot = chart_library_tasks::ProgressSnapshot;
  std::uint64_t displayedLibraryTasksRevision = 0;
  std::uint64_t displayedLibraryProgressRevision = 0;
  std::string displayedLibraryTasksButtonText;
  struct SelectedChartRandomInfo {
    std::optional<unsigned int> seed;
    std::optional<std::string> prng;
    std::optional<std::vector<int>> values;
  };
  struct LibraryFolderItem {
    enum class Type {
      AllSongs,
      Favorites,
      SolidArchives,
      DifficultyTable,
      DifficultyLevel,
      DifficultyClearMark,
      CoursesRoot,
      CourseTable,
      CourseGroup,
      Course
    };

    std::string key;
    std::string label;
    Type type = Type::AllSongs;
    int depth = 0;
    int count = -1;
    int tableId = 0;
    std::string tableLevel;
    int courseId = 0;
    std::string courseKey;
    int courseTableId = 0;
    std::string courseGroupName;
    std::string courseConstraintJson;
    int clearRank = kNoClearTypeRank;
    int clearMarkRank = kNoClearTypeRank;
    bool clearMarkFolder = false;
    bool expandable = false;
    bool expanded = false;
  };

  RecyclerView<ChartMetaRecord> *recyclerView = nullptr;
  RecyclerView<LibraryFolderItem> *folderRecyclerView = nullptr;
  struct LibraryFolderMetadataCache {
    bool valid = false;
    std::uint64_t libraryRevision = 0;
    int allSongCount = 0;
    int favoriteCount = 0;
    int solidArchiveCount = 0;
    std::vector<DifficultyTableInfo> tables;
    std::unordered_map<int, std::vector<DifficultyLevelInfo>> levelsByTable;
    std::vector<DifficultyCourseTableInfo> courseTables;
    std::unordered_map<int, std::vector<DifficultyCourseGroupInfo>>
        courseGroupsByTable;
    std::unordered_map<std::string, std::vector<DifficultyCourseInfo>>
        coursesByGroup;
  };
  struct ChartListPageCache {
    ChartRepository::Session *session = nullptr;
    ChartMetaQuery query;
    int totalCount = 0;
    int pageSize = 128;
    int maxPages = 6;
    mutable std::unordered_map<int, std::vector<ChartMetaRecord>> pages;
    mutable std::deque<int> pageOrder;
    mutable ChartMetaRecord fallbackRecord;
    std::optional<ChartMetaRecord> leadingRecord;
    void reset(ChartRepository::Session &chartSession,
               const ChartMetaQuery &chartQuery, int count,
               std::optional<ChartMetaRecord> leading = std::nullopt);
    void releasePages();
    void clear();
    [[nodiscard]] const ChartMetaRecord &get(int index) const;

  private:
    void touchPage(int pageIndex) const;
  };
  ChartListPageCache chartListCache;
  View *rootLayout = nullptr;
  OverlayPortal *overlayPortal = nullptr;
  DecideLoadingOverlay *decideOverlay_ = nullptr;
  std::unique_ptr<ContextMenuView> revealContextMenu;
  std::unique_ptr<ReplayRecordsModal> recordsModal_;
  ImageView *jacketView = nullptr;
  TextInputBox *searchBox = nullptr;
  ChartFilterPanelView *chartFilterPanel = nullptr;
  ChartSortPanelView *chartSortPanel = nullptr;
  Button *chartFilterButton = nullptr;
  TextView *chartFilterButtonText = nullptr;
  Button *chartSortButton = nullptr;
  TextView *chartSortButtonText = nullptr;
  Button *startButton = nullptr;
  Button *rankingsButton = nullptr;
  TextView *rankingsButtonText = nullptr;
  std::unique_ptr<ir::IrRankingModal> rankingsModal;
  View *chartActionsRow = nullptr;
  Button *revealButton = nullptr;
  View *replayButtonSlot = nullptr;
  Button *replayButton = nullptr;
  View *findBmsButtonSlot = nullptr;
  Button *findBmsButton = nullptr;
  TextView *findBmsButtonText = nullptr;
  View *unzipButtonSlot = nullptr;
  Button *unzipButton = nullptr;
  TextView *unzipButtonText = nullptr;
  Button *parseLogButton = nullptr;
  TextView *parseLogButtonText = nullptr;
  Button *musicButton = nullptr;
  TextView *musicButtonText = nullptr;
  Button *irUploadsButton = nullptr;
  TextView *irUploadsButtonText = nullptr;
  Button *tasksButton = nullptr;
  TextView *tasksButtonText = nullptr;
  TextView *replayButtonText = nullptr;
  TextView *replayStatusText = nullptr;
  TextView *startButtonText = nullptr;
  View *playOptionsModalRoot = nullptr;
  PlayOptionsPanelView *playOptionsPanel = nullptr;
  std::unique_ptr<MainMenuPlayOptionsModal> playOptionsModal;
  View *parseLogModalRoot = nullptr;
  View *musicModalRoot = nullptr;
  View *tasksModalRoot = nullptr;
  View *unzipModalRoot = nullptr;
  View *unzipProgressTrack = nullptr;
  View *unzipProgressFill = nullptr;
  TextView *unzipModalTitleText = nullptr;
  TextView *unzipProgressMessageText = nullptr;
  TextView *unzipProgressPercentText = nullptr;
  TextView *unzipProgressDetailText = nullptr;
  Button *unzipDeleteArchiveButton = nullptr;
  Button *unzipCancelButton = nullptr;
  TextView *unzipDeleteArchiveButtonText = nullptr;
  TextView *unzipCancelButtonText = nullptr;
  RecyclerView<MainMenuParseLogRow> *parseLogRecyclerView = nullptr;
  TextView *parseLogExportStatusText = nullptr;
  Button *parseLogExportButton = nullptr;
  TextView *parseLogExportButtonText = nullptr;
  Button *parseLogCloseButton = nullptr;
  TextView *parseLogCloseButtonText = nullptr;
  TextView *musicTrackText = nullptr;
  TextView *musicStatusText = nullptr;
  TextView *musicPlaylistText = nullptr;
  Button *musicSelectedButton = nullptr;
  Button *musicAddSelectedButton = nullptr;
  Button *musicRemoveSelectedButton = nullptr;
  Button *musicPlaylistButton = nullptr;
  Button *musicClearPlaylistButton = nullptr;
  Button *musicRandomButton = nullptr;
  Button *musicPreviousButton = nullptr;
  Button *musicSeekBackwardButton = nullptr;
  Button *musicPlayPauseButton = nullptr;
  Button *musicSeekForwardButton = nullptr;
  Button *musicNextButton = nullptr;
  Button *musicStopButton = nullptr;
  Button *musicCloseButton = nullptr;
  TextView *musicSelectedButtonText = nullptr;
  TextView *musicAddSelectedButtonText = nullptr;
  TextView *musicRemoveSelectedButtonText = nullptr;
  TextView *musicPlaylistButtonText = nullptr;
  TextView *musicClearPlaylistButtonText = nullptr;
  TextView *musicRandomButtonText = nullptr;
  TextView *musicPreviousButtonText = nullptr;
  TextView *musicSeekBackwardButtonText = nullptr;
  TextView *musicPlayPauseButtonText = nullptr;
  TextView *musicSeekForwardButtonText = nullptr;
  TextView *musicNextButtonText = nullptr;
  TextView *musicStopButtonText = nullptr;
  TextView *musicCloseButtonText = nullptr;
  ScrollView *tasksScrollView = nullptr;
  View *tasksContent = nullptr;
  TextView *tasksText = nullptr;
  Button *tasksRefreshButton = nullptr;
  TextView *tasksRefreshButtonText = nullptr;
  Button *tasksCloseButton = nullptr;
  TextView *tasksCloseButtonText = nullptr;
  View *findBmsModalRoot = nullptr;
  View *findBmsProgressTrack = nullptr;
  View *findBmsProgressFill = nullptr;
  TextView *findBmsModalTitleText = nullptr;
  TextView *findBmsStatusText = nullptr;
  TextView *findBmsDetailText = nullptr;
  Button *findBmsCloseButton = nullptr;
  Button *findBmsKeepFilesButton = nullptr;
  Button *findBmsDeleteFilesButton = nullptr;
  Button *findBmsOpenButton = nullptr;
  Button *findBmsGoogleButton = nullptr;
  Button *findBmsRefreshButton = nullptr;
  RecyclerView<BmsSearchCandidate> *findBmsCandidateRecyclerView = nullptr;
  TextView *findBmsCloseButtonText = nullptr;
  TextView *findBmsKeepFilesButtonText = nullptr;
  TextView *findBmsDeleteFilesButtonText = nullptr;
  TextView *findBmsOpenButtonText = nullptr;
  TextView *findBmsGoogleButtonText = nullptr;
  TextView *findBmsRefreshButtonText = nullptr;
  TextView *readyGaugeText = nullptr;
  View *readyTotalRow = nullptr;
  TextView *readyTotalIconText = nullptr;
  TextView *readyTotalText = nullptr;
  TextView *readyPlayOptionText = nullptr;
  TextView *readyAssistOptionText = nullptr;
  TextView *readyPacemakerText = nullptr;
  Button *readyPlayOptionsButton = nullptr;
  Button *playOptionsCloseButton = nullptr;
  TextView *playOptionsCloseButtonText = nullptr;
  struct PendingReplayExportResult {
    bool success = false;
    std::filesystem::path outputPath;
    std::string message;
  };
  std::mutex replayExportResultMutex;
  std::optional<PendingReplayExportResult> pendingReplayExportResult;
  struct PendingReplayExportProgress {
    double fraction = 0.0;
    std::string message;
  };
  std::mutex replayExportProgressMutex;
  std::optional<PendingReplayExportProgress> pendingReplayExportProgress;
  struct PendingUnzipResult {
    bool success = false;
    std::filesystem::path chartPath;
    std::filesystem::path rootPath;
    std::filesystem::path outputFolder;
    std::filesystem::path archivePath;
    std::string message;
    bool canDeleteArchive = false;
  };
  struct PendingUnzipProgress {
    double fraction = 0.0;
    std::uint64_t current = 0;
    std::uint64_t total = 0;
    std::string message;
  };
  std::mutex unzipResultMutex;
  std::optional<PendingUnzipResult> pendingUnzipResult;
  std::mutex unzipProgressMutex;
  std::optional<PendingUnzipProgress> pendingUnzipProgress;
  std::optional<std::filesystem::path> pendingSelectChartPath;
  struct PendingFindBmsSelectionHandoff {
    std::filesystem::path chartPath;
    main_menu_library::FindBmsChartIdentity targetIdentity;
    std::uint64_t selectionGeneration = 0;
  };
  std::mutex findBmsSelectionHandoffMutex;
  std::optional<PendingFindBmsSelectionHandoff>
      pendingFindBmsSelectionHandoff;
  std::optional<std::filesystem::path> suppressPreviewForChartPath;
  std::optional<std::filesystem::path> unzipDeleteCandidatePath;
  std::uint64_t unzipEstimatedUncompressedSize = 0;
  std::atomic_bool findBmsJobRunning = false;
  std::atomic_bool findBmsCancelled = false;
  ChartMetaRecord findBmsModalChart;
  BmsSearchResult findBmsResult;
  std::optional<BmsSearchPendingArtifactDecision> findBmsPendingDecision;
  std::string findBmsProgressMessage;
  std::uint64_t findBmsProgressCurrent = 0;
  std::uint64_t findBmsProgressTotal = 0;
  double findBmsProgressFraction = 0.0;
  std::deque<std::string> findBmsProgressLog;
  std::mutex findBmsUpdateMutex;
  std::deque<BmsSearchDownloadProgress> pendingFindBmsProgressEvents;
  std::optional<BmsSearchResult> pendingFindBmsResult;
  std::uint64_t chartSelectionGeneration = 0;
  std::uint64_t findBmsSelectionGenerationAtDownloadStart = 0;

  LibraryFolderItem activeFolder;
  LibraryFolderMetadataCache folderMetadataCache;
  ScoreClearRankCache scoreClearRanks;
  ScoreBestCache scoreBestScores;
  std::uint64_t scoreClearRanksRevision = 0;
  std::uint64_t observedIrReconciliationRevision = 0;
  std::uint64_t observedIrAccountEvidenceRevision = 0;
  std::uint64_t libraryRevision = 0;
  chart_library::FolderClearDataByLongNoteMode folderClearData;
  struct CourseValidationCache {
    bool valid = false;
    std::uint64_t libraryRevision = 0;
    int courseId = 0;
    bool empty = true;
    int firstMissingIndex = -1;
    std::vector<ChartMetaRecord> records;
  };
  struct CurrentCourseSelection {
    std::vector<ChartMetaRecord> records;
    std::vector<std::filesystem::path> completedChartPaths;
    bool completeCourse = false;
  };
  CourseValidationCache courseValidationCache;
  std::unordered_set<std::string> expandedLibraryFolders;
  std::string searchText;
  std::optional<std::filesystem::path> temporaryChartFolder;
  std::optional<ChartMetaRecord> selectedChartRecord;
  ChartRecordFilters chartRecordFilters;
  bool chartFilterPanelVisible = false;
  bool chartSortPanelVisible = false;
  std::string chartBpmMinText;
  std::string chartBpmMaxText;
  bool chartClearMarkDropdownOpen = false;
  bool chartScoreRankDropdownOpen = false;
  bool chartDifficultyMinDropdownOpen = false;
  bool chartDifficultyMaxDropdownOpen = false;
  std::optional<int> chartDifficultyRangeTableId;
  std::string publishedResultRecordDiagnostic;
  platform_document_handoff::PlatformDocumentHandoffOperation
      replayFileDocumentHandoff;
  platform_document_handoff::PlatformDocumentHandoffOperation
      parseLogDocumentHandoff;
  main_menu_profile::Selections profileSelections;
  bool profileSelectionsInitialized = false;
  struct EffectivePlayOptionSelection {
    std::string playOption = "NORMAL";
    std::string longNoteMode = AppSettings::kDefaultLnMode;
    std::string assistOption = assist_options::kOff;
    bool longNoteModeLocked = false;
    bool assistOptionLocked = false;
  };
  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
  int lastSafeTop = -1;
  int lastSafeLeft = -1;
  int lastSafeBottom = -1;
  int lastSafeRight = -1;
  std::uint64_t parseLogDisplayedRevision = 0;
  ui_theme::ThemeMode appliedUiThemeMode = ui_theme::ThemeMode::Dark;
  std::string musicStatusMessage;

  void initView(ApplicationContext &context);
  void applyThemeChange();
  void reloadProfileSelectionsFromSettings();
  void reloadFolderItems(bool preserveViewState = false);
  void refreshFavoriteFolderCount();
  ChartMetaQuery chartQueryForActiveFolder() const;
  bool chartDifficultyRangeEnabled() const;
  std::vector<DifficultyLevelInfo> chartFilterDifficultyLevels() const;
  void setChartFilterPanelVisible(bool visible);
  void setChartSortPanelVisible(bool visible);
  void refreshChartFilterPanel();
  void refreshChartFilterButtons();
  void setChartClearFilter(std::optional<int> rank);
  void setChartScoreRankFilter(std::optional<std::string> rank);
  void setChartBpmMinFilter(const std::string &text);
  void setChartBpmMaxFilter(const std::string &text);
  void setChartDifficultyMinFilter(std::optional<std::string> level);
  void setChartDifficultyMaxFilter(std::optional<std::string> level);
  void setChartClearMarkDropdownOpen(bool open);
  void setChartScoreRankDropdownOpen(bool open);
  void setChartClearMarkRange(bool orAbove, bool orBelow);
  void setChartScoreRankRange(bool orAbove, bool orBelow);
  void setChartDifficultyDropdownOpen(bool minLevel, bool open);
  void setChartSortCriterion(ChartRecordSortCriterion criterion);
  void reloadChartList(bool preserveViewState = false);
  void reloadChartListForFolderSelection();
  std::optional<std::string> reloadScoreClearRanks();
  std::optional<std::string> prepareScoreQueryDatabase();
  std::optional<std::string> refreshScoreClearRankViews();
  void refreshLongNoteModeClearRankViews();
  void refreshScoreClearRanksIfNeeded();
  void refreshIrRecordListIfNeeded();
  void refreshLibraryIfNeeded();
  void startLibraryRefresh();
  void startLibraryRebuild();
  void enqueueLibraryRefreshTask(
      const std::string &title,
      const std::filesystem::path &folderToAdd = std::filesystem::path(),
      const std::string &iosBookmark = "",
      bool rebuildLibraryMetadata = false);
  void enqueueDownloadedPathIndexTask(
      const std::filesystem::path &path,
      const main_menu_library::FindBmsChartIdentity &targetIdentity = {},
      std::uint64_t selectionGeneration = 0,
      std::vector<std::filesystem::path> removedPaths = {});
  LibraryTaskProgressSnapshot readLibraryTaskProgress() const;
  int activeLibraryTaskCount();
  void requestLibraryScanFlush();
  void refreshTasksButton();
  int clearRankForChart(const ChartMetaRecord &record) const;
  int clearRankForFolder(const std::string &key) const;
  int clearMarkCountForFolder(const std::string &key, int clearMarkRank) const;
  void requestLibraryReload(bool includeFolders);
  void applyPendingUiUpdates();
  void selectFolder(LibraryFolderItem item);
  bool toggleChartFavorite(const ChartMetaRecord &record, bool favorite);
  void setGameplayRulesetSelection(GameplayRuleset ruleset);
  void setGaugeSelection(GaugeType gaugeType, GaugeAutoShiftMode autoShift);
  void setGaugeAutoShiftLowerBound(GaugeType gaugeType);
  void refreshGaugeSelectionButtons();
  void setPlayOptionSelection(const std::string &option);
  void refreshPlayOptionButtons();
  void setLongNoteModeSelection(const std::string &mode);
  void refreshLongNoteModeButtons();
  void setAssistOptionSelection(const std::string &option);
  void refreshAssistOptionButtons();
  void setPacemakerTargetSelection(const std::string &target);
  void refreshPacemakerTargetButtons();
  void setPlaybackRateSelection(int percent);
  void setPlaybackModeSelection(const std::string &mode);
  void toggleGameplayClubMode();
  void refreshPlaybackSelectionControls();
  void refreshPlayOptionsPanel();
  [[nodiscard]] bool playbackSelectionLockedForCourse() const;
  bool currentAssistOptionSelectionAllowed(const std::string &option) const;
  std::optional<ChartMetaRecord> selectedRecordSnapshot() const;
  void refreshSelectedChartActionState();
  void refreshRankingsButton();
  void openRankingsForSelection();
  EffectivePlayOptionSelection currentEffectivePlayOptionSelection() const;
  bool currentPlayOptionSelectionAllowed(const std::string &option) const;
  bool currentLongNoteModeSelectionAllowed(const std::string &mode) const;
  void refreshReadySettingsSummary();
  bms_parser::Chart *setSelectedChart(std::unique_ptr<bms_parser::Chart> chart,
                                      bool mediaReady,
                                      bool reusableForStart = true);
  void clearSelectedChart();
  void schedulePreviewLoad(bms_parser::ChartMeta meta);
  void startPreviewLoadThread(bms_parser::ChartMeta meta,
                              DebounceToken previewToken);
  void cancelActivePreviewLoading();
  void retirePreviewLoadThread(bool stopPreviewAudioWhenDone);
  void reapRetiredPreviewLoadThreads();
  void joinRetiredPreviewLoadThreads();
  void cancelPreviewLoading(bool stopPreviewAudio);
  void stopAndClearSelectedChart();
  SelectedChartRandomInfo
  selectedChartRandomInfoForPath(const std::filesystem::path &path) const;
  bms_parser::Chart *
  loadedSelectedChartForPath(const std::filesystem::path &path) const;
  void resetStartLoadingUi();
  void resetReplayWatchLoadingUi();
  void publishReplayLoadDiagnostic(const char *action,
                                   const std::string &diagnostic) const;
  bool finishReplayLoadFailure(const char *action, std::string diagnostic,
                               const char *fallback);
  void changeToGameplayScene(bms_parser::Chart *chart, StartOptions options);
  void startSelectedChart();
  void startChartDirect(const ChartMetaRecord &record);
  const CourseValidationCache &courseValidationForActiveFolder();
  [[nodiscard]] std::optional<CurrentCourseSelection>
  currentCourseSelectionFor(
      const result_persistence::ModernCourseResult &result);
  void refreshStartButtonForActiveFolder();
  void startSelectedCourse();
  void startCourseDirect(std::shared_ptr<CoursePlaySession> session);
  void openChartViewerForSelection();
  void openChartViewerDirect(const ChartMetaRecord &record);
  void toggleRevealContextMenu();
  void showSelectedChartFolder();
  bool clearSameFolderScope();
  void revealSelectedChartInFileManager();
  void startUnzipSelectedArchiveFolder();
  void startUnzipArchiveFolder(const ChartMetaRecord &record);
  void setPlayableChartActionsVisible(bool visible);
  void setPlayableChartActionsVisible(bool visible, bool chartActionsVisible);
  void setUnzipButtonVisible(bool visible);
  void refreshUnzipButtonForSelection(const ChartMetaRecord *record);
  void buildUnzipProgressModal();
  void showUnzipProgressModal();
  void hideUnzipProgressModal();
  void updateUnzipProgressUi(double fraction, const std::string &message,
                             std::uint64_t current, std::uint64_t total);
  void setUnzipDeleteArchiveButtonVisible(bool visible);
  void deleteUnzippedSourceArchive();
  void applyUnzipProgress();
  void applyUnzipResult();
  enum class AutoSelectionPreview { Load, Suppress };
  void selectChartByPathAfterReload(const std::filesystem::path &path,
                                    AutoSelectionPreview preview);
  void setFindBmsButtonVisible(bool visible);
  void openFindBmsForSelection();
  void buildParseLogModal();
  void showParseLogModal();
  void hideParseLogModal();
  void refreshParseLogModal(bool forceScrollToBottom = false);
  void startParseLogExport();
  void applyParseLogDocumentHandoff();
  void refreshParseLogExportControls();
  [[nodiscard]] bool isParseLogScrolledNearBottom() const;
  void scrollParseLogModalToBottom();
  void buildMusicModal();
  void showMusicModal();
  void hideMusicModal();
  void refreshMusicModal();
  void playSelectedChartAsMusic();
  void addSelectedChartToMusicPlaylist();
  void removeSelectedChartFromMusicPlaylist();
  void playSavedMusicPlaylist();
  void clearSavedMusicPlaylist();
  void playRandomMusicLibrary();
  void toggleMusicPlayback();
  void seekMusicRelative(long long deltaMicros);
  void playNextMusicTrack();
  void playPreviousMusicTrack();
  void stopMusicPlayback();
  void buildTasksModal();
  void showTasksModal();
  void hideTasksModal();
  void refreshTasksModal();
  std::string tasksModalTextSnapshot();
  void buildFindBmsModal();
  void showFindBmsModal(const ChartMetaRecord &record);
  void startFindBmsCandidateDownload(size_t candidateIndex);
  void startFindBmsPendingArtifactResolution(
      BmsSearchPendingArtifactDecision decision);
  void hideFindBmsModal();
  void refreshFindBmsModal();
  void applyFindBmsUpdates();
  void openFindBmsResultUrl(const std::string &url);
  std::filesystem::path preferredBmsDownloadRoot();
  void reselectCurrentChart();
  void refreshReplayAvailability(const ChartMetaRecord *record);
  void setReplayButtonVisible(bool visible);
  void buildPlayOptionsModal();
  void showPlayOptionsModal();
  void hidePlayOptionsModal();
  void openReplayRecordsForSelection();
  void shareReplayFile(const replay::ReplayFileActionRequest &request);
  void removeReplayFile(const replay::ReplayFileActionRequest &request);
  void applyReplayFileDocumentHandoff();
  ReplayRecordsModalCallbacks makeRecordsModalCallbacks();
  std::vector<ResultRecordSummary>
  loadRecordsForModal(const ChartMetaRecord &record);
  bool beginReplayExport(const std::string &progressTitle,
                         const std::string &progressMessage,
                         const std::string &statusMessage);
  void queueReplayExportResult(const ReplayVideoExportResult &result);
  bms_parser::ChartMeta
  replayLoadMetaForRecord(const ChartMetaRecord &record) const;
  ReplaySummary autoPlayReplaySummary(const ChartMetaRecord &record) const;
  bool prepareAutoPlayChartForRecord(
      const ChartMetaRecord &record,
      std::unique_ptr<bms_parser::Chart> &preparedChart,
      play_options::PlayOptionReplayInfo &playInfo,
      std::atomic_bool &parseCancelled) const;
  void startAutoPlayPlayback(const ChartMetaRecord &record);
  void startModernReplayPlayback(const ChartMetaRecord &record,
                                 ModernChartResultRecord modern);
  void startModernCourseReplayPlayback(const ChartMetaRecord &record,
                                       ModernCourseResultRecord modern);
  void startModernGBattlePlayback(const ChartMetaRecord &record,
                                  ModernChartResultRecord modern);
  void startCourseReplayDirect(std::shared_ptr<CoursePlaySession> session);
  void startAutoPlayVideoExport(const ChartMetaRecord &record,
                                ReplayVideoExportOptions options);
  void startModernReplayVideoExport(const ChartMetaRecord &record,
                                    ModernChartResultRecord modern,
                                    ReplayVideoExportOptions options);
  void startModernCourseReplayVideoExport(
      ModernCourseResultRecord modern, ReplayVideoExportOptions options);
  void startModernReplayResultRecall(const ChartMetaRecord &record,
                                     ModernChartResultRecord modern);
  void startModernCourseReplayResultRecall(ModernCourseResultRecord modern,
                                           bool retrySameAllowed);
  void startRemoteResultRecall(IrRemoteRecordId identity,
                               std::string selectedStableKey);
  void startModernReplayIrUpload(ModernChartResultRecord modern);
  void finishReplayIrUpload(std::string attemptId, std::string message);
  void publishReplayIrStatusFeedback(ir::IrRecordState state);
  void observeReplayIrServiceRevisions();
  [[nodiscard]] std::optional<std::string>
  activeReplayIrServerOrigin() const;
  void finishReplayResultRecallFailure(std::string diagnostic = {});
  void finishRemoteResultRecallFailure(std::string diagnostic = {});
  void startReplayLoadWorker(
      std::function<void(std::shared_ptr<std::atomic_bool>)> work);
  void queueReplayLoadCompletion(std::function<void()> completion);
  void applyReplayLoadCompletion();
  void stopReplayLoadWorker();
  void applyReplayExportProgress();
  void applyReplayExportResult();
  enum DiffType { Deleted, Added };
  struct Diff {
    std::filesystem::path path;
    DiffType type;
  };
#ifdef _WIN32
  static void FindFilesWin(const std::filesystem::path &path,
                           std::vector<Diff> &diffs,
                           const std::unordered_set<path_t> &oldFilesWs,
                           std::vector<path_t> &directoriesToVisit,
                           const std::stop_token &stop_token);
#elif TARGET_OS_OSX || TARGET_OS_LINUX || TARGET_OS_ANDROID
  static void
  FindFilesUnix(const std::filesystem::path &path, std::vector<Diff> &diffs,
                const std::unordered_set<path_t> &oldFilesWs,
                std::vector<std::filesystem::path> &directoriesToVisit,
                const std::stop_token &stop_token);
#elif TARGET_OS_IPHONE
  static void
  FindFilesIOS(const std::filesystem::path &path, std::vector<Diff> &diffs,
               const std::unordered_set<path_t> &oldFilesWs,
               std::vector<std::filesystem::path> &directoriesToVisit,
               const std::stop_token &stop_token);
#endif
  static void FindNewBmsFiles(std::vector<Diff> &diffs,
                              const std::unordered_set<path_t> &oldFilesWs,
                              const std::filesystem::path &path,
                              const std::stop_token &stop_token);
  static void resolveDType(const std::filesystem::path &directoryPath,
                           struct dirent *entry);
};
