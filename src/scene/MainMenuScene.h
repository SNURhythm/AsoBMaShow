#pragma once
#include "../BmsSearchService.h"
#include "../view/RecyclerView.h"
#include "Scene.h"
#include "../ChartDBHelper.h"
#include "../ReplayDBHelper.h"
#include "../ReplayVideoExporter.h"
#include "../ScoreDBHelper.h"
#include "../path.h"
#include "../view/ImageView.h"
#include "../view/ReplaySummaryListView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include <filesystem>
#include <deque>
#include <thread>
#include <unordered_set>
#include <vector>
#include "../targets.h"
#include "../audio/Jukebox.h"
#include "../video/VideoPlayer.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>

class Button;
class ScrollView;
class View;

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
  sqlite3 *db;
  std::atomic_bool previewLoadCancelled = false;
  std::atomic_bool willStart = false;
  std::atomic<bms_parser::Chart *> selectedChart{nullptr};
  std::atomic_bool selectedChartMediaReady = false;

  std::thread loadThread;
  std::jthread checkEntriesThread;
  std::jthread addFolderPickerThread;
  std::jthread findBmsThread;
  std::jthread replayExportThread;
  std::jthread unzipThread;
  std::atomic_bool folderItemsReloadRequested = false;
  std::atomic_bool chartListReloadRequested = false;
  std::atomic_bool replayExportInProgress = false;
  std::atomic_bool unzipInProgress = false;
  std::atomic_bool addFolderPickerInProgress = false;
  std::atomic_bool libraryTaskWorkerPaused = false;
  enum class LibraryTaskStatus {
    Queued,
    Running,
    Complete,
    Failed,
    Paused,
  };
  struct LibraryTaskRequest {
    std::uint64_t id = 0;
    std::string title;
    std::filesystem::path folderToAdd;
    std::string iosBookmark;
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
  struct LibraryFolderItem {
    enum class Type {
      AllSongs,
      SolidArchives,
      DifficultyTable,
      DifficultyLevel,
      CoursesRoot,
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
    int courseTableId = 0;
    std::string courseGroupName;
    int clearRank = kNoClearTypeRank;
  };

  RecyclerView<ChartMetaRecord> *recyclerView = nullptr;
  RecyclerView<LibraryFolderItem> *folderRecyclerView = nullptr;
  struct ChartListPageCache {
    sqlite3 *db = nullptr;
    ChartMetaQuery query;
    int totalCount = 0;
    int pageSize = 128;
    int maxPages = 6;
    mutable std::unordered_map<int, std::vector<ChartMetaRecord>> pages;
    mutable std::deque<int> pageOrder;
    mutable ChartMetaRecord fallbackRecord;

    void reset(sqlite3 *database, const ChartMetaQuery &chartQuery, int count);
    void clear();
    [[nodiscard]] const ChartMetaRecord &get(int index) const;

  private:
    void touchPage(int pageIndex) const;
  };
  ChartListPageCache chartListCache;
  View *rootLayout = nullptr;
  ImageView *jacketView = nullptr;
  TextInputBox *searchBox = nullptr;
  TextInputBox *difficultyFilterBox = nullptr;
  Button *startButton = nullptr;
  View *chartActionsRow = nullptr;
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
  Button *tasksButton = nullptr;
  TextView *tasksButtonText = nullptr;
  TextView *replayButtonText = nullptr;
  TextView *replayStatusText = nullptr;
  View *replayModalRoot = nullptr;
  View *replayModalContentFrame = nullptr;
  View *replayListContent = nullptr;
  View *replayExportOptionsContent = nullptr;
  View *replayExportProgressContent = nullptr;
  View *replayExportProgressTrack = nullptr;
  View *replayExportProgressFill = nullptr;
  TextView *replayModalTitleText = nullptr;
  TextView *replayExportProgressMessageText = nullptr;
  TextView *replayExportProgressPercentText = nullptr;
  TextView *startButtonText = nullptr;
  View *playOptionsModalRoot = nullptr;
  View *parseLogModalRoot = nullptr;
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
  ScrollView *parseLogScrollView = nullptr;
  View *parseLogContent = nullptr;
  TextView *parseLogText = nullptr;
  Button *parseLogCloseButton = nullptr;
  TextView *parseLogCloseButtonText = nullptr;
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
  ScrollView *findBmsLogScrollView = nullptr;
  View *findBmsLogContent = nullptr;
  TextView *findBmsLogText = nullptr;
  Button *findBmsCloseButton = nullptr;
  Button *findBmsOpenButton = nullptr;
  Button *findBmsGoogleButton = nullptr;
  Button *findBmsRefreshButton = nullptr;
  RecyclerView<BmsSearchCandidate> *findBmsCandidateRecyclerView = nullptr;
  TextView *findBmsCloseButtonText = nullptr;
  TextView *findBmsOpenButtonText = nullptr;
  TextView *findBmsGoogleButtonText = nullptr;
  TextView *findBmsRefreshButtonText = nullptr;
  TextView *readyGaugeText = nullptr;
  TextView *readyPlayOptionText = nullptr;
  Button *playOptionsCloseButton = nullptr;
  TextView *playOptionsCloseButtonText = nullptr;
  ReplaySummaryListView *replayListView = nullptr;
  Button *replayWatchButton = nullptr;
  Button *replayModalExportButton = nullptr;
  Button *replayModalCloseButton = nullptr;
  Button *replayFps60Button = nullptr;
  Button *replayFps120Button = nullptr;
  Button *replayResolution1080Button = nullptr;
  Button *replayResolutionFullButton = nullptr;
  TextView *replayWatchButtonText = nullptr;
  TextView *replayModalExportButtonText = nullptr;
  TextView *replayModalCloseButtonText = nullptr;
  TextView *replayFps60ButtonText = nullptr;
  TextView *replayFps120ButtonText = nullptr;
  TextView *replayResolution1080ButtonText = nullptr;
  TextView *replayResolutionFullButtonText = nullptr;
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
  std::optional<std::filesystem::path> suppressPreviewForChartPath;
  std::optional<std::filesystem::path> unzipDeleteCandidatePath;
  std::uint64_t unzipEstimatedUncompressedSize = 0;
  std::atomic_bool findBmsJobRunning = false;
  std::atomic_bool findBmsCancelled = false;
  ChartMetaRecord findBmsModalChart;
  BmsSearchResult findBmsResult;
  std::string findBmsProgressMessage;
  std::uint64_t findBmsProgressCurrent = 0;
  std::uint64_t findBmsProgressTotal = 0;
  double findBmsProgressFraction = 0.0;
  std::deque<std::string> findBmsProgressLog;
  std::mutex findBmsUpdateMutex;
  std::deque<BmsSearchDownloadProgress> pendingFindBmsProgressEvents;
  std::optional<BmsSearchResult> pendingFindBmsResult;

  LibraryFolderItem activeFolder;
  ScoreClearRankCache scoreClearRanks;
  std::uint64_t scoreClearRanksRevision = 0;
  std::uint64_t libraryRevision = 0;
  std::unordered_map<std::string, int> folderClearRanks;
  std::string searchText;
  std::string difficultyText;
  std::vector<ReplaySummary> replaySummaries;
  ChartMetaRecord replayModalChart;
  int selectedReplayIndex = -1;
  int selectedExportFps = 120;
  bool selectedExportFullResolution = true;
  double replayExportProgressFraction = 0.0;
  struct GaugeSelectionButton {
    Button *button = nullptr;
    TextView *text = nullptr;
    GaugeType type = GaugeType::Normal;
    bool autoShift = false;
  };
  std::vector<GaugeSelectionButton> gaugeSelectionButtons;
  GaugeType selectedGaugeType = GaugeType::Normal;
  bool selectedGaugeAutoShift = false;
  struct PlayOptionButton {
    Button *button = nullptr;
    TextView *text = nullptr;
    std::string option;
  };
  std::vector<PlayOptionButton> playOptionButtons;
  std::string selectedPlayOption = "NORMAL";
  int lastLayoutWidth = -1;
  int lastLayoutHeight = -1;
  int lastSafeTop = -1;
  int lastSafeLeft = -1;
  int lastSafeBottom = -1;
  int lastSafeRight = -1;
  std::uint64_t parseLogDisplayedRevision = 0;

  void initView(ApplicationContext &context);
  void reloadFolderItems(bool preserveViewState = false);
  void reloadChartList(bool preserveViewState = false);
  void reloadScoreClearRanks();
  void refreshScoreClearRankViews();
  void refreshScoreClearRanksIfNeeded();
  void refreshLibraryIfNeeded();
  void startLibraryRefresh();
  void startLibraryTaskWorker();
  void stopLibraryTaskWorker();
  void pauseLibraryTaskWorker();
  void resumeLibraryTaskWorker();
  bool waitForLibraryTaskResume(std::uint64_t id,
                                const std::stop_token &stopToken);
  void enqueueLibraryRefreshTask(
      const std::string &title,
      const std::filesystem::path &folderToAdd = std::filesystem::path(),
      const std::string &iosBookmark = "");
  void libraryTaskLoop(const std::stop_token &stopToken);
  void runLibraryRefreshTask(const LibraryTaskRequest &task,
                             const std::stop_token &stopToken);
  void setLibraryTaskState(std::uint64_t id, LibraryTaskStatus status,
                           double fraction, int current, int total,
                           const std::string &detail);
  void updateLibraryTaskProgress(std::uint64_t id,
                                 const ChartScanProgress &progress);
  LibraryTaskProgressSnapshot readLibraryTaskProgress() const;
  int activeLibraryTaskCount();
  void requestLibraryScanFlush();
  std::uint64_t pendingLibraryScanFlushRequest() const;
  void completeLibraryScanFlush(std::uint64_t request);
  void refreshTasksButton();
  int clearRankForChart(const ChartMetaRecord &record) const;
  int clearRankForFolder(const std::string &key) const;
  void requestLibraryReload(bool includeFolders);
  void applyPendingUiUpdates();
  void selectFolder(const LibraryFolderItem &item);
  void setGaugeSelection(GaugeType gaugeType, bool autoShift);
  void refreshGaugeSelectionButtons();
  void setPlayOptionSelection(const std::string &option);
  void refreshPlayOptionButtons();
  void refreshReadySettingsSummary();
  bms_parser::Chart *loadedSelectedChart() const;
  void startSelectedChart();
  void startChartDirect(const ChartMetaRecord &record);
  void openChartViewerForSelection();
  void openChartViewerDirect(const ChartMetaRecord &record);
  void revealSelectedChartInFileManager();
  void startUnzipSelectedArchiveFolder();
  void startUnzipArchiveFolder(const ChartMetaRecord &record);
  void setPlayableChartActionsVisible(bool visible);
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
  void refreshParseLogModal();
  void buildTasksModal();
  void showTasksModal();
  void hideTasksModal();
  void refreshTasksModal();
  std::string tasksModalTextSnapshot();
  void buildFindBmsModal();
  void showFindBmsModal(const ChartMetaRecord &record);
  void startFindBmsCandidateDownload(size_t candidateIndex);
  void hideFindBmsModal();
  void refreshFindBmsModal();
  void applyFindBmsUpdates();
  void openFindBmsResultUrl(const std::string &url);
  std::filesystem::path preferredBmsDownloadRoot();
  void refreshReplayAvailability(const ChartMetaRecord *record);
  void setReplayButtonVisible(bool visible);
  void buildPlayOptionsModal();
  void showPlayOptionsModal();
  void hidePlayOptionsModal();
  void buildReplayModal();
  void showReplayListModal(const ChartMetaRecord &record);
  void showReplayExportOptions();
  void showReplayExportProgress();
  void hideReplayModal();
  void refreshReplayModalActions();
  void refreshReplayExportOptionButtons();
  void updateReplayExportProgressUi(double fraction,
                                    const std::string &message);
  void startReplayPlayback(const ChartMetaRecord &record, int replayId);
  void startReplayVideoExport(const ChartMetaRecord &record, int replayId,
                              ReplayVideoExportOptions options);
  void applyReplayVideoExportProgress();
  void applyReplayVideoExportResult();
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  void addIOSFolderEntryFromFiles();
#endif
  static void LoadCharts(ChartDBHelper &dbHelper, sqlite3 *db,
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
#elif TARGET_OS_OSX || TARGET_OS_LINUX
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
