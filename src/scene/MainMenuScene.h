#pragma once
#include "../BmsSearchService.h"
#include "../view/RecyclerView.h"
#include "Scene.h"
#include "../ChartDBHelper.h"
#include "../ReplayDBHelper.h"
#include "../ReplayVideoExporter.h"
#include "../ScoreDBHelper.h"
#include "../ThreadCompat.h"
#include "../path.h"
#include "../view/ImageView.h"
#include "../view/ReplaySummaryListView.h"
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
class ScrollView;
struct CoursePlaySession;
struct StartOptions;
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
  sqlite3 *db = nullptr;
  std::atomic_bool previewLoadCancelled = false;
  std::atomic_bool willStart = false;
  std::unique_ptr<bms_parser::Chart> selectedChart;
  mutable std::mutex selectedChartMutex;
  std::atomic_bool selectedChartMediaReady = false;
  std::atomic_bool selectedChartReusableForStart = false;

  std::thread loadThread;
  std::jthread checkEntriesThread;
  std::jthread addFolderPickerThread;
  std::jthread archiveImportPickerThread;
  std::jthread findBmsThread;
  std::jthread replayExportThread;
  std::jthread unzipThread;
  std::atomic_bool folderItemsReloadRequested = false;
  std::atomic_bool chartListReloadRequested = false;
  std::atomic_bool replayExportInProgress = false;
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
    std::filesystem::path androidImportPath;
    bool androidImportFolder = false;
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
    std::string courseConstraintJson;
    int clearRank = kNoClearTypeRank;
    int clearMarkRank = kNoClearTypeRank;
    bool clearMarkFolder = false;
    bool expandable = false;
    bool expanded = false;
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
    std::optional<ChartMetaRecord> leadingRecord;

    void reset(sqlite3 *database, const ChartMetaQuery &chartQuery, int count,
               std::optional<ChartMetaRecord> leading = std::nullopt);
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
  Button *musicButton = nullptr;
  TextView *musicButtonText = nullptr;
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
  ScrollView *parseLogScrollView = nullptr;
  View *parseLogContent = nullptr;
  TextView *parseLogText = nullptr;
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
  TextView *readyAssistOptionText = nullptr;
  Button *playOptionsCloseButton = nullptr;
  TextView *playOptionsCloseButtonText = nullptr;
  ReplaySummaryListView *replayListView = nullptr;
  Button *replayWatchButton = nullptr;
  Button *replayModalPhotoButton = nullptr;
  Button *replayModalExportButton = nullptr;
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
  TextView *replayModalPhotoButtonText = nullptr;
  TextView *replayModalExportButtonText = nullptr;
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
    bool photo = false;
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
  main_menu_library::FolderClearDataByLongNoteMode folderClearData;
  std::unordered_set<std::string> expandedLibraryFolders;
  std::string searchText;
  std::string difficultyText;
  std::vector<ReplaySummary> replaySummaries;
  ChartMetaRecord replayModalChart;
  int selectedReplayIndex = -1;
  int selectedExportFps = 120;
  bool selectedExportFullResolution = true;
  bool selectedExportIncludeResultScreen = true;
  bool selectedReplayRenderTouchPoints = true;
  bool selectedReplayRenderGhosts = true;
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
  struct LongNoteModeButton {
    Button *button = nullptr;
    TextView *text = nullptr;
    std::string mode;
  };
  std::vector<LongNoteModeButton> longNoteModeButtons;
  std::string selectedLnMode = AppSettings::kDefaultLnMode;
  struct AssistOptionButton {
    Button *button = nullptr;
    TextView *text = nullptr;
    std::string option;
  };
  std::vector<AssistOptionButton> assistOptionButtons;
  std::string selectedAssistOption = assist_options::kOff;
  struct EffectivePlayOptionSelection {
    std::string playOption = "NORMAL";
    std::string longNoteMode = AppSettings::kDefaultLnMode;
    bool playOptionLocked = false;
    bool longNoteModeLocked = false;
    std::string playOptionLockSource;
    std::string longNoteModeLockSource;
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
  void reloadFolderItems(bool preserveViewState = false);
  void reloadChartList(bool preserveViewState = false);
  void reloadScoreClearRanks();
  void rebuildScoreClearRankTempTable();
  void refreshScoreClearRankViews();
  void refreshLongNoteModeClearRankViews();
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
      sqlite3 *taskDb, std::uint64_t taskId,
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
  int clearRankForChart(const ChartMetaRecord &record) const;
  int clearRankForFolder(const std::string &key) const;
  int clearMarkCountForFolder(const std::string &key, int clearMarkRank) const;
  void requestLibraryReload(bool includeFolders);
  void applyPendingUiUpdates();
  void selectFolder(const LibraryFolderItem &item);
  bool toggleChartFavorite(const ChartMetaRecord &record, bool favorite);
  void setGaugeSelection(GaugeType gaugeType, bool autoShift);
  void refreshGaugeSelectionButtons();
  void setPlayOptionSelection(const std::string &option);
  void refreshPlayOptionButtons();
  void setLongNoteModeSelection(const std::string &mode);
  void refreshLongNoteModeButtons();
  void setAssistOptionSelection(const std::string &option);
  void refreshAssistOptionButtons();
  std::optional<ChartMetaRecord> selectedRecordSnapshot() const;
  EffectivePlayOptionSelection currentEffectivePlayOptionSelection() const;
  bool currentPlayOptionSelectionAllowed(const std::string &option) const;
  bool currentLongNoteModeSelectionAllowed(const std::string &mode) const;
  void refreshReadySettingsSummary();
  bms_parser::Chart *setSelectedChart(std::unique_ptr<bms_parser::Chart> chart,
                                      bool mediaReady,
                                      bool reusableForStart = true);
  void clearSelectedChart();
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
  void refreshStartButtonForActiveFolder();
  void startSelectedCourse();
  void startCourseDirect(std::shared_ptr<CoursePlaySession> session);
  void openChartViewerForSelection();
  void openChartViewerDirect(const ChartMetaRecord &record);
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
  void refreshParseLogModal();
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
  void showReplayExportOptions();
  void showReplayExportProgress(const std::string &title = "Exporting Replay",
                                const std::string &message =
                                    "Preparing export");
  void hideReplayModal();
  void refreshReplayModalActions();
  void refreshReplayExportOptionButtons();
  void updateReplayExportProgressUi(double fraction,
                                    const std::string &message);
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
  void startCourseReplayPlayback(const ChartMetaRecord &record, int replayId);
  void startCourseReplayDirect(std::shared_ptr<CoursePlaySession> session);
  void startReplayVideoExport(const ChartMetaRecord &record, int replayId,
                              ReplayVideoExportOptions options);
  void startReplayImageExport(const ChartMetaRecord &record, int replayId);
  void applyReplayVideoExportProgress();
  void applyReplayVideoExportResult();
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
