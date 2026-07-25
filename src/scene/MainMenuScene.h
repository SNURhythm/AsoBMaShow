#pragma once
#include "../BmsSearchService.h"
#include "../ChartLibraryScanner.h"
#include "../view/RecyclerView.h"
#include "ChartFilterSortPanelView.h"
#include "Scene.h"
#include "../repositories/ChartRepository.h"
#include "../repositories/ReplayRepository.h"
#include "../ReplayRecordFilters.h"
#include "../ResultRecordSummary.h"
#include "../ReplayVideoExporter.h"
#include "../PlatformDocumentHandoff.h"
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
#include <thread>
#include <unordered_set>
#include <vector>
#include "../targets.h"
#include "../audio/Jukebox.h"
#include "../video/VideoPlayer.h"
#include "MainMenuLibrary.h"
#include "MainMenuProfileSelections.h"
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
  struct RetiredPreviewLoadThread {
    std::thread thread;
    std::shared_ptr<std::atomic_bool> finished;
  };
  std::shared_ptr<std::atomic_bool> previewLoadCancelToken =
      std::make_shared<std::atomic_bool>(false);
  std::shared_ptr<std::atomic_bool> previewLoadFinishedToken =
      std::make_shared<std::atomic_bool>(true);
  Debouncer previewLoadDebouncer;
  std::vector<RetiredPreviewLoadThread> retiredPreviewLoadThreads;
  std::mutex retiredPreviewLoadThreadsMutex;
  std::mutex previewJukeboxLoadMutex;
  bool pendingStopAndClearSelectedChartAfterPreview = false;
  std::jthread checkEntriesThread;
  std::jthread addFolderPickerThread;
  std::jthread archiveImportPickerThread;
  std::jthread findBmsThread;
  std::jthread replayExportThread;
  std::jthread unzipThread;
  std::atomic_bool folderItemsReloadRequested = false;
  std::atomic_bool chartListReloadRequested = false;
  std::atomic_bool replayExportInProgress = false;
  bool replayFileActionInProgress = false;
  bool replayDeleteConfirmationPending = false;
  platform_document_handoff::PlatformDocumentHandoffOperation
      replayDocumentHandoff;
  bool replayResultRecallInProgress = false;
  bool replayIrUploadInProgress = false;
  std::uint64_t replayIrUploadFeedbackRevision = 0;
  std::unordered_map<std::string, std::uint64_t> replayIrObservedRevisions;
  std::atomic_bool unzipInProgress = false;
  std::atomic_bool addFolderPickerInProgress = false;
  std::atomic_bool archiveImportPickerInProgress = false;
  std::atomic_bool libraryTaskWorkerPaused = false;
  std::atomic_bool tasksModalOpenRequested = false;
  enum class LibraryTaskStatus {
    Queued,
    Running,
    Complete,
    Failed,
    Paused,
  };
  enum class LibraryTaskKind {
    RefreshLibrary,
    AndroidImport,
  };
  struct LibraryTaskRequest {
    std::uint64_t id = 0;
    LibraryTaskKind kind = LibraryTaskKind::RefreshLibrary;
    std::string title;
    std::filesystem::path folderToAdd;
    std::string iosBookmark;
    std::filesystem::path additionalFolderToScan;
    std::filesystem::path androidImportPath;
    bool androidImportFolder = false;
    bool rebuildLibraryMetadata = false;
  };
  struct LibraryTaskInfo {
    std::uint64_t id = 0;
    std::string title;
    LibraryTaskStatus status = LibraryTaskStatus::Queued;
    double fraction = 0.0;
    int current = 0;
    int total = 0;
    std::string detail;
  };
  std::deque<LibraryTaskRequest> libraryTaskQueue;
  std::vector<LibraryTaskInfo> libraryTasks;
  std::mutex libraryTaskMutex;
  std::mutex libraryTaskWorkerMutex;
  std::mutex libraryTaskPauseMutex;
  std::condition_variable_any libraryTaskCv;
  std::condition_variable_any libraryTaskPauseCv;
  std::atomic<std::uint64_t> nextLibraryTaskId{1};
  std::uint64_t libraryTasksRevision = 0;
  std::atomic<int> libraryActiveTaskCount{0};
  std::uint64_t displayedLibraryTasksRevision = 0;
  std::atomic<std::uint64_t> libraryProgressRevision{0};
  std::atomic<std::uint64_t> libraryProgressTaskId{0};
  std::atomic<int> libraryProgressCurrent{0};
  std::atomic<int> libraryProgressTotal{0};
  std::atomic<int> libraryProgressBasisPoints{0};
  std::atomic<int> libraryProgressStage{
      static_cast<int>(ChartScanProgressStage::Preparing)};
  std::atomic<std::uint64_t> libraryScanFlushRequested{0};
  std::atomic<std::uint64_t> libraryScanFlushCompleted{0};
  std::uint64_t displayedLibraryProgressRevision = 0;
  std::string displayedLibraryTasksButtonText;
  struct LibraryTaskProgressSnapshot {
    bool valid = false;
    std::uint64_t revision = 0;
    std::uint64_t taskId = 0;
    int current = 0;
    int total = 0;
    int basisPoints = 0;
    ChartScanProgressStage stage = ChartScanProgressStage::Preparing;
  };
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
  std::unique_ptr<ContextMenuView> revealContextMenu;
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
  View *replayModalRoot = nullptr;
  View *replayModalContentFrame = nullptr;
  View *replayListContent = nullptr;
  View *replayFilterSortContent = nullptr;
  View *replayWatchOptionsContent = nullptr;
  View *replayExportOptionsContent = nullptr;
  View *replayExportProgressContent = nullptr;
  View *replayExportProgressTrack = nullptr;
  View *replayExportProgressFill = nullptr;
  TextView *replayModalTitleText = nullptr;
  TextView *replayExportProgressMessageText = nullptr;
  TextView *replayExportProgressPercentText = nullptr;
  TextView *startButtonText = nullptr;
  View *playOptionsModalRoot = nullptr;
  PlayOptionsPanelView *playOptionsPanel = nullptr;
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
  ResultRecordListView *replayListView = nullptr;
  Button *replayWatchButton = nullptr;
  Button *replayGBattleButton = nullptr;
  Button *replayModalResultButton = nullptr;
  Button *replayModalExportButton = nullptr;
  Button *replayModalShareButton = nullptr;
  Button *replayModalDeleteButton = nullptr;
  Button *replayModalFilterButton = nullptr;
  Button *replayModalCloseButton = nullptr;
  Button *replayFps60Button = nullptr;
  Button *replayFps120Button = nullptr;
  Button *replayResolution1080Button = nullptr;
  Button *replayResolutionFullButton = nullptr;
  Button *replayResultIncludeButton = nullptr;
  Button *replayResultSkipButton = nullptr;
  Button *replayTouchShowButton = nullptr;
  Button *replayTouchHideButton = nullptr;
  Button *replayGhostShowButton = nullptr;
  Button *replayGhostHideButton = nullptr;
  Button *replayExportTouchShowButton = nullptr;
  Button *replayExportTouchHideButton = nullptr;
  Button *replayExportGhostShowButton = nullptr;
  Button *replayExportGhostHideButton = nullptr;
  TextView *replayWatchButtonText = nullptr;
  TextView *replayGBattleButtonText = nullptr;
  TextView *replayModalResultButtonText = nullptr;
  TextView *replayModalExportButtonText = nullptr;
  TextView *replayModalShareButtonText = nullptr;
  TextView *replayModalDeleteButtonText = nullptr;
  TextView *replayModalFilterButtonText = nullptr;
  TextView *replayModalCloseButtonText = nullptr;
  TextView *replayFps60ButtonText = nullptr;
  TextView *replayFps120ButtonText = nullptr;
  TextView *replayResolution1080ButtonText = nullptr;
  TextView *replayResolutionFullButtonText = nullptr;
  TextView *replayResultIncludeButtonText = nullptr;
  TextView *replayResultSkipButtonText = nullptr;
  TextView *replayTouchShowButtonText = nullptr;
  TextView *replayTouchHideButtonText = nullptr;
  TextView *replayGhostShowButtonText = nullptr;
  TextView *replayGhostHideButtonText = nullptr;
  TextView *replayExportTouchShowButtonText = nullptr;
  TextView *replayExportTouchHideButtonText = nullptr;
  TextView *replayExportGhostShowButtonText = nullptr;
  TextView *replayExportGhostHideButtonText = nullptr;
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
  std::mutex androidArchiveImportMutex;
  std::optional<std::string> pendingAndroidArchiveImportError;
  std::deque<std::pair<std::uint64_t, bool>> pendingAndroidArchiveImportTasks;
  std::atomic_bool androidArchiveImportCopyPending = false;
  std::uint64_t nextAndroidArchiveImportPollMs = 0;
  std::optional<std::filesystem::path> pendingSelectChartPath;
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
  std::filesystem::path findBmsDownloadRoot;

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
  std::vector<ReplaySummary> replaySummaries;
  std::vector<ResultRecordSummary> resultRecordSummaries;
  std::vector<ResultRecordSummary> visibleResultRecordSummaries;
  std::optional<std::string> selectedResultRecordStableKey;
  std::optional<ResultRecordSummary> selectedResultRecordSummary;
  std::string publishedResultRecordDiagnostic;
  ReplayRecordFilters replayRecordFilters;
  ChartMetaRecord replayModalChart;
  std::optional<ReplaySummary> selectedReplaySummary;
  std::optional<ReplaySummary> replayExportSelection;
  ChartMetaRecord replayExportChart;
  int selectedReplayIndex = -1;
  int selectedExportFps = 120;
  bool selectedExportFullResolution = true;
  bool selectedExportIncludeResultScreen = true;
  bool selectedReplayRenderTouchPoints = true;
  bool selectedReplayRenderGhosts = true;
  double replayExportProgressFraction = 0.0;
  struct ReplayClearFilterButton {
    Button *button = nullptr;
    TextView *text = nullptr;
    std::optional<int> rank;
  };
  struct ReplayOptionFilterButton {
    Button *button = nullptr;
    TextView *text = nullptr;
    std::optional<std::string> option;
  };
  struct ReplayScoreRankFilterButton {
    Button *button = nullptr;
    TextView *text = nullptr;
    std::optional<std::string> rank;
  };
  struct ReplaySortButton {
    Button *button = nullptr;
    TextView *text = nullptr;
    ReplayRecordSortCriterion criterion = ReplayRecordSortCriterion::Newest;
  };
  std::vector<ReplayClearFilterButton> replayClearFilterButtons;
  std::vector<ReplayOptionFilterButton> replayPlayOptionFilterButtons;
  std::vector<ReplayScoreRankFilterButton> replayScoreRankFilterButtons;
  std::vector<ReplaySortButton> replaySortButtons;
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
  std::optional<std::string> reloadScoreClearRanks();
  std::optional<std::string> prepareScoreQueryDatabase();
  std::optional<std::string> refreshScoreClearRankViews();
  void refreshLongNoteModeClearRankViews();
  void refreshScoreClearRanksIfNeeded();
  void refreshIrRecordListIfNeeded();
  void refreshLibraryIfNeeded();
  void startLibraryRefresh(
      const std::filesystem::path &additionalFolderToScan = {});
  void startLibraryRebuild();
  void startLibraryTaskWorker();
  void stopLibraryTaskWorker();
  void pauseLibraryTaskWorker();
  void resumeLibraryTaskWorker();
  void syncLibraryTaskPauseStateWithForegroundScene();
  bool waitForLibraryTaskResume(std::uint64_t id,
                                const std::stop_token &stopToken);
  void enqueueLibraryRefreshTask(
      const std::string &title,
      const std::filesystem::path &folderToAdd = std::filesystem::path(),
      const std::string &iosBookmark = "",
      bool rebuildLibraryMetadata = false,
      const std::filesystem::path &additionalFolderToScan = {});
#if TARGET_OS_ANDROID
  void createPendingAndroidImportTask(bool folderImport);
  void enqueueAndroidImportTask(std::uint64_t id,
                                const std::filesystem::path &importPath,
                                bool folderImport);
#endif
  void libraryTaskLoop(const std::stop_token &stopToken);
  void runLibraryRefreshTask(const LibraryTaskRequest &task,
                             const std::stop_token &stopToken);
#if TARGET_OS_ANDROID
  void runAndroidImportTask(const LibraryTaskRequest &task,
                            const std::stop_token &stopToken);
#endif
  void seedDefaultDifficultyTablesIfNeeded(
      ChartRepository::Session &chartSession, std::uint64_t taskId,
      const std::stop_token &stopToken);
  static bool isPauseableLibraryTaskStatus(LibraryTaskStatus status);
  static bool isActiveLibraryTaskStatus(LibraryTaskStatus status);
  void setLibraryTaskState(std::uint64_t id, LibraryTaskStatus status,
                           double fraction, int current, int total,
                           const std::string &detail);
  void bumpLibraryTasksRevisionLocked();
  void updateLibraryTaskProgress(std::uint64_t id,
                                 const ChartScanProgress &progress);
  LibraryTaskProgressSnapshot readLibraryTaskProgress() const;
  int activeLibraryTaskCount();
  void requestLibraryScanFlush();
  std::uint64_t pendingLibraryScanFlushRequest() const;
  void completeLibraryScanFlush(std::uint64_t request);
  void refreshTasksButton();
  bool insertChartFolderEntryImmediately(
      const std::filesystem::path &folderPath,
      const std::string &iosBookmark);
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
  void changeToGameplayScene(bms_parser::Chart *chart, StartOptions options);
  void startSelectedChart();
  void startChartDirect(const ChartMetaRecord &record);
  const CourseValidationCache &courseValidationForActiveFolder();
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
  void selectChartByPathAfterReload(const std::filesystem::path &path);
  void setFindBmsButtonVisible(bool visible);
  void openFindBmsForSelection();
  void buildParseLogModal();
  void showParseLogModal();
  void hideParseLogModal();
  void refreshParseLogModal(bool forceScrollToBottom = false);
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
  void buildReplayModal();
  void showReplayListModal(const ChartMetaRecord &record);
  void reloadReplayRecordModels(bool preserveViewState);
  void showReplayFilterSortOptions();
  void showReplayExportOptions();
  void showReplayExportProgress(const std::string &title = "Exporting Replay",
                                const std::string &message =
                                    "Preparing export");
  void hideReplayModal();
  void refreshReplayModalActions();
  void refreshReplayFilterSortButtons();
  void refreshReplayExportOptionButtons();
  void updateReplayExportProgressUi(double fraction,
                                    const std::string &message);
  void clearReplayModalSelection();
  bool selectReplayModalIndex(int index);
  void applyReplayRecordFilters(
      std::optional<std::string> preferredStableKey = std::nullopt);
  void setReplayClearFilter(std::optional<int> rank);
  void setReplayPlayOptionFilter(std::optional<std::string> option);
  void setReplayScoreRankFilter(std::optional<std::string> rank);
  void setReplaySortCriterion(ReplayRecordSortCriterion criterion);
  bool replayScoreRankFilterAvailable() const;
  bool beginReplayExport(const std::string &progressTitle,
                         const std::string &progressMessage,
                         const std::string &statusMessage);
  void queueReplayExportResult(const ReplayVideoExportResult &result);
  bool selectedReplayIsAutoPlay() const;
  bool selectedReplayIsCourseReplay() const;
  bms_parser::ChartMeta
  replayLoadMetaForRecord(const ChartMetaRecord &record) const;
  ReplaySummary autoPlayReplaySummary(const ChartMetaRecord &record) const;
  bool prepareAutoPlayChartForRecord(
      const ChartMetaRecord &record,
      std::unique_ptr<bms_parser::Chart> &preparedChart,
      play_options::PlayOptionReplayInfo &playInfo,
      std::atomic_bool &parseCancelled) const;
  void startReplayPlayback(const ChartMetaRecord &record, int replayId);
  void startGBattlePlayback(const ChartMetaRecord &record, int replayId);
  void startCourseReplayPlayback(const ChartMetaRecord &record, int replayId);
  void startCourseReplayDirect(std::shared_ptr<CoursePlaySession> session);
  void startReplayVideoExport(const ChartMetaRecord &record, int replayId,
                              ReplayVideoExportOptions options);
  void startReplayFileShare();
  void requestReplayFileDeletion();
  void applyReplayDocumentHandoffResult();
  void startReplayResultRecall(const ChartMetaRecord &record, int replayId);
  void startRemoteResultRecall(IrRemoteRecordId identity,
                               std::string selectedStableKey);
  void startReplayIrUpload(const ChartMetaRecord &record,
                           ReplaySummary summary);
  void finishReplayIrUpload(int replayId, std::string message);
  void publishReplayIrStatusFeedback(ir::IrRecordState state);
  void observeReplayIrServiceRevisions();
  [[nodiscard]] std::optional<std::string>
  activeReplayIrServerOrigin() const;
  void refreshReplayIrMarker(
      int replayId,
      ir::IrRecordActivity activity = ir::IrRecordActivity::None);
  void startCourseReplayResultRecall(int replayId);
  void finishReplayResultRecallFailure(std::string diagnostic = {});
  void finishRemoteResultRecallFailure(std::string diagnostic = {});
  void applyReplayExportProgress();
  void applyReplayExportResult();
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  void addIOSFolderEntryFromFiles();
#endif
#if TARGET_OS_ANDROID
  void addAndroidFolderEntryFromPicker();
  void importAndroidArchiveFromPicker();
  void importAndroidFolderFromPicker();
  void importAndroidPathFromPicker(bool folderImport);
  void pollPendingAndroidArchiveImport();
  void applyPendingAndroidArchiveImport();
#endif
  static void LoadCharts(ChartRepository::Session &chartSession,
                         std::vector<ChartEntry> &entries, MainMenuScene &scene,
                         const std::stop_token &stop_token,
                         ChartScanProgressCallback progressCallback = nullptr,
                         ChartScanPauseCallback pauseCallback = nullptr);
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
