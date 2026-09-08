#include <atomic>
#include <cassert>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

void SDL_Log(const char *, ...) {}
std::string fspath_to_utf8(const std::filesystem::path &path) { return path.string(); }
namespace ir { std::string sanitizeDiagnostic(const std::string &value) { return value; } }
std::string replayDiagnosticOr(const std::string &value, const char *fallback) {
  return value.empty() ? fallback : value;
}
struct View {
  bool visible = true;
  std::string text;
  void setVisible(bool value) { visible = value; }
  void setText(const std::string &value) { text = value; }
  void dismiss() {}
  void close() {}
};
struct ResultRecordSummary {};
struct ReplayRecordsModal {
  View root;
  View *root_ = &root, *deleteConfirmationContent_ = nullptr;
  View *watchButtonText_ = nullptr, *gbattleButtonText_ = nullptr;
  View *resultButtonText_ = nullptr, *exportButtonText_ = nullptr;
  struct Confirmation { void cancel() {} } deleteConfirmation_;
  std::optional<int> exportSelection_;
  bool exportInProgress_ = false, resultRecallInProgress_ = false;
  bool irUploadInProgress_ = false, loadInProgress_ = false, documentHandoffActive_ = false;
  std::string status;
  void refreshActions() {}
  void clearSelection() {}
  void setStatus(const std::string &value) { status = value; }
  void reloadRecords(bool) {}
  void returnToList(const std::string &value) { status = value; }
  void showExportProgress(const std::string &, const std::string &) { root.visible = true; }
  auto selection() { return std::optional<ResultRecordSummary>(ResultRecordSummary{}); }
  void hide();
  void setLoadInProgress(bool);
  void setResultRecallInProgress(bool);
  void setExportInProgress(bool);
  bool operationInProgress() const noexcept;
  bool canHide() const noexcept;
};
MODAL_METHODS

namespace bms_parser {
struct ChartMeta { std::filesystem::path BmsPath = "chart.bms"; };
struct Chart {
  inline static int alive = 0;
  ChartMeta Meta;
  Chart() { ++alive; }
  ~Chart() { --alive; }
};
}
struct ChartMetaRecord {
  bms_parser::ChartMeta meta;
  bool courseStart = false, unavailable = false;
};
struct ModernChartResultRecord {};
struct ModernCourseResultRecord { int result = 0; };
struct IrRemoteRecordId {
  std::string providerId = "tachi", serverOrigin = "https://example.test", remoteScoreId = "123";
};
namespace play_options {
struct PlayOptionReplayInfo {
  std::string option = "OFF";
  int seed = 12;
  std::string option2 = "MIRROR";
  int seed2 = 34;
};
}
namespace audio { struct PlaybackRate { int percent = 100, mode = 0; }; }
namespace pacemaker { constexpr int kTargetOff = -1; }
namespace long_note_mode { int valueFromId(int mode) { return mode; } }
using GameplayRuleset = int;
struct StartOptions {
  int startPosition;
  bool autoKeySound, autoPlay;
  int gaugeType, gaugeAutoShift, gaugeAutoShiftLowerBound;
  std::string playOption;
  int playOptionSeed;
  std::string playOption2;
  int playOption2Seed;
  int longNoteMode, assistOption, pacemakerTarget;
  audio::PlaybackRate playback;
  bool touchVisualizationEnabled, replayGhostRenderingEnabled;
  GameplayRuleset ruleset;
};
struct ProfileSelections {
  int gaugeType = 1, gaugeAutoShift = 2, gaugeAutoShiftLowerBound = 3;
  int longNoteMode = 1, assistOption = 4, pacemakerTarget = 5;
  GameplayRuleset ruleset = 6;
};
struct ReplayData {};
struct ScoreProvenance {};
struct SkinGameplayGraphState {};
namespace replay_result {
SkinGameplayGraphState BuildSkinGameplayGraphState(const auto &, const auto &, const auto &) { return {}; }
SkinGameplayGraphState BuildSkinGameplayChartGraphState(const auto &, const auto &) { return {}; }
}
struct ChartCompletion {
  struct Result {
    std::unique_ptr<bms_parser::Chart> chart = std::make_unique<bms_parser::Chart>();
    int state = 0;
    struct {
      std::string attemptId = "attempt";
      struct { ScoreProvenance provenance; } score;
      int playedAtUnixMillis = 123;
    } result;
  } view;
  std::shared_ptr<ReplayData> retryData = std::make_shared<ReplayData>();
};
struct CoursePlaySession {
  struct Result { bms_parser::ChartMeta meta; int state; SkinGameplayGraphState gameplayGraph; };
  std::vector<Result> completedResults{Result{}};
  std::vector<std::optional<ScoreProvenance>> stageProvenance{ScoreProvenance{}};
  int modernCoursePlayedAtUnixMillis = 123;
  const ReplayData *resultBrowseStageReplay(int) { return nullptr; }
  bms_parser::Chart *resultBrowseReplayChart(int) { return nullptr; }
  void applyReplayStagePlayOptions(const ReplayData &) {}
};
struct ResultPersistenceOptions {};
struct ResultPracticeOptions {};
enum class ResultCourseMode { Stage };
struct ResultCourseOptions {
  ResultCourseMode mode;
  std::shared_ptr<CoursePlaySession> session;
  bool savedResultBrowsing;
};
struct ResultTableContext {};
struct ResultRemoteOptions {};
struct ResultScene { template<class... Arguments> ResultScene(Arguments &&...) {} };
struct SceneManager {
  int transitions = 0;
  std::function<void()> pause, resume;
  void changeScene(std::unique_ptr<ResultScene>, bool retained) {
    assert(retained);
    pause();
    ++transitions;
  }
  void returnToRetainedOwner() { resume(); }
};
struct RemoteResultRecallRequest { IrRemoteRecordId identity; std::string selectedStableKey; };
struct RemoteResultRecallCallbacks {
  std::function<bool(const RemoteResultRecallRequest &)> selectionStillMatches;
  std::function<bool(const IrRemoteRecordId &)> loadExact;
  std::function<bool(ResultRemoteOptions, bool)> transition;
  std::function<void(std::string)> failAndReload;
};
bool remoteSelectionMatches = true;
bool remoteResultRecallSelectionMatches(const std::optional<ResultRecordSummary> &,
                                       const RemoteResultRecallRequest &) { return remoteSelectionMatches; }
void executeRemoteResultRecall(const RemoteResultRecallRequest &request, RemoteResultRecallCallbacks &callbacks) {
  if (!callbacks.selectionStillMatches(request) || !callbacks.loadExact(request.identity)) {
    callbacks.failAndReload("remote unavailable");
    return;
  }
  callbacks.transition({}, true);
}
struct PreviewWorker { void cancel() {} void stop() {} };
struct ReplayVideoExportResult { bool success; std::filesystem::path outputPath; std::string message; };
struct Recycler {
  int selectedIndex = -1;
  int size() { return 0; }
  ChartMetaRecord get(int) { return {}; }
  std::function<void(const ChartMetaRecord &, int)> onSelected;
};
struct Callbacks {
  std::function<void(const ChartMetaRecord &)> watchAutoPlay;
  std::function<void(const ChartMetaRecord &, const ModernChartResultRecord &)> recallModernChart;
  std::function<void(const ModernCourseResultRecord &, bool)> recallModernCourse;
  std::function<void(const IrRemoteRecordId &, const std::string &)> recallRemote;
};
struct MainMenuScene {
  SceneManager manager;
  struct {
    struct { int selectedPlaybackRatePercent = 125, selectedPlaybackMode = 1; } settings;
    struct { void *scene = nullptr, *background = nullptr; } profileSwitchBlockers;
    struct { bool available = true; bool LoadIrRemoteScore(const auto &...) { return available; } } replayRepository;
    struct {
      bool cancelled = false;
      void stop() {}
      void loadChart(bms_parser::Chart &, bool, std::atomic_bool &cancel) { cancel = cancelled; }
    } jukebox;
    SceneManager *sceneManager;
  } context;
  ProfileSelections profileSelections;
  ReplayRecordsModal modal;
  ReplayRecordsModal *recordsModal_ = &modal;
  PreviewWorker *previewWorker_ = nullptr;
  std::mutex previewCleanupMutex;
  bool pendingStopAndClearSelectedChartAfterPreview = false;
  std::atomic_bool willStart = false, replayExportInProgress = false;
  bool replayResultRecallInProgress = false, replayIrUploadInProgress = false;
  std::atomic_bool replayLoadInProgress = false;
  std::jthread replayLoadThread, replayExportThread;
  std::shared_ptr<std::atomic_bool> replayLoadCancelToken = std::make_shared<std::atomic_bool>(false);
  std::mutex replayLoadCompletionMutex, replayExportProgressMutex, replayExportResultMutex;
  std::function<void()> pendingReplayLoadCompletion;
  std::optional<int> pendingReplayExportProgress;
  using PendingReplayExportResult = ReplayVideoExportResult;
  std::optional<PendingReplayExportResult> pendingReplayExportResult;
  std::atomic_bool selectedChartMediaReady = true, selectedChartReusableForStart = true;
  Recycler *recyclerView = nullptr;
  View *replayStatusText = nullptr, *revealContextMenu = nullptr, *rankingsModal = nullptr;
  struct Cache { void releasePages() {} } chartListCache;
  std::vector<int> replayIrObservedRevisions;
  int scoreClearRanks = 0, scoreBestScores = 0, folderClearData = 0, scoreClearRanksRevision = 0;
  std::atomic_bool preparationEntered = false, releasePreparation = false;
  bool preparationFails = false, selectFails = false;
  std::unique_ptr<bms_parser::Chart> selectedChart;
  std::optional<StartOptions> startOptions;
  std::vector<std::function<bool()>> deferred;
  MainMenuScene() {
    context.sceneManager = &manager;
    manager.pause = [this] { onPause(); };
    manager.resume = [this] { onResume(); };
  }
  ~MainMenuScene() { stopReplayLoadWorker(); }
  void defer(std::function<bool()> callback, int, bool) { deferred.push_back(std::move(callback)); }
  void drain() {
    auto callbacks = std::move(deferred);
    deferred.clear();
    for (auto &callback : callbacks) callback();
  }
  bool prepareAutoPlayChartForRecord(const ChartMetaRecord &, std::unique_ptr<bms_parser::Chart> &chart,
                                    play_options::PlayOptionReplayInfo &, std::atomic_bool &) {
    if (preparationFails) return false;
    chart = std::make_unique<bms_parser::Chart>();
    return true;
  }
  bms_parser::Chart *setSelectedChart(std::unique_ptr<bms_parser::Chart> chart, bool, bool) {
    if (selectFails) return nullptr;
    selectedChart = std::move(chart);
    return selectedChart.get();
  }
  void changeToGameplayScene(bms_parser::Chart *, StartOptions options) {
    startOptions = std::move(options);
    manager.pause();
    ++manager.transitions;
  }
  auto currentCourseSelectionFor(int) { return std::optional<int>(1); }
  void applyThemeChange() {}
  auto prepareScoreQueryDatabase() { return std::optional<int>{}; }
  void reloadProfileSelectionsFromSettings() {}
  auto refreshScoreClearRankViews() { return std::optional<int>{}; }
  void refreshLongNoteModeClearRankViews() {}
  void refreshLibraryIfNeeded() {}
  void reselectCurrentChart() {}
  Callbacks callbacks() { Callbacks callbacks; OWNER_CALLBACKS return callbacks; }
  void startAutoPlayPlayback(const ChartMetaRecord &);
  void resetReplayWatchLoadingUi();
  void startModernReplayResultRecall(const ChartMetaRecord &, ModernChartResultRecord);
  void startModernCourseReplayResultRecall(ModernCourseResultRecord, bool);
  void startRemoteResultRecall(IrRemoteRecordId, std::string);
  void finishReplayResultRecallFailure(std::string);
  void finishRemoteResultRecallFailure(std::string);
  void startReplayLoadWorker(std::function<void(std::shared_ptr<std::atomic_bool>)>);
  void queueReplayLoadCompletion(std::function<void()>);
  void applyReplayLoadCompletion();
  void stopReplayLoadWorker();
  bool beginReplayExport(const std::string &, const std::string &, const std::string &);
  void applyReplayExportResult();
  void onPause();
  void onResume();
};
OWNER_METHODS

int failures = 0;
void expect(bool condition, const char *message) {
  if (!condition) { ++failures; std::cerr << message << '\n'; }
}
void testAutoPlay() {
  for (int outcome = 0; outcome < 4; ++outcome) {
    MainMenuScene scene;
    scene.preparationFails = outcome == 1;
    scene.context.jukebox.cancelled = outcome == 2;
    scene.selectFails = outcome == 3;
    scene.callbacks().watchAutoPlay({});
    expect(scene.willStart && scene.modal.operationInProgress(), "AutoPlay entry must lock owner/modal");
    scene.modal.hide();
    expect(scene.modal.root.visible, "busy AutoPlay must block dismissal");
    scene.drain();
    expect(!scene.willStart && !scene.modal.operationInProgress(), "AutoPlay completion must release owner/modal");
    expect(scene.manager.transitions == (outcome == 0), "AutoPlay must transition only on success");
    if (outcome == 0) {
      expect(!scene.modal.root.visible, "AutoPlay success must clear loading before hiding");
      scene.manager.returnToRetainedOwner();
      expect(!scene.modal.root.visible && scene.modal.canHide(), "AutoPlay return must not reopen a stuck modal");
      expect(scene.startOptions->autoPlay && scene.startOptions->playOptionSeed == 12 &&
             scene.startOptions->playback.percent == 125, "AutoPlay options must survive");
    } else {
      expect(scene.modal.root.visible && scene.modal.canHide(), "AutoPlay rejection must keep recoverable Records");
      expect(bms_parser::Chart::alive == 0, "AutoPlay rejection must release prepared chart");
    }
  }
}
void testRecall() {
  for (int path = 0; path < 3; ++path) {
    for (int outcome = 0; outcome < 3; ++outcome) {
      MainMenuScene scene;
      scene.preparationFails = outcome == 1;
      scene.context.replayRepository.available = outcome != 1;
      remoteSelectionMatches = outcome != 2;
      const auto callbacks = scene.callbacks();
      if (path == 0) callbacks.recallModernChart({}, {});
      if (path == 1) callbacks.recallModernCourse({}, true);
      if (path == 2) callbacks.recallRemote({}, "remote");
      expect(scene.replayResultRecallInProgress && scene.modal.operationInProgress(), "recall entry must lock owner/modal");
      scene.modal.hide();
      expect(scene.modal.root.visible, "recall must block dismissal until completion");
      if (path < 2) {
        while (!scene.preparationEntered) std::this_thread::yield();
        if (outcome == 2) scene.stopReplayLoadWorker();
        else {
          scene.releasePreparation = true;
          scene.replayLoadThread.join();
          scene.applyReplayLoadCompletion();
        }
      } else scene.drain();
      expect(!scene.replayResultRecallInProgress && !scene.modal.operationInProgress(), "recall success/failure/cancel must release both flags");
      expect(scene.manager.transitions == (outcome == 0), "recall must transition only on success");
      if (outcome == 0) scene.manager.returnToRetainedOwner();
      expect(scene.modal.root.visible && scene.modal.canHide(), "result return or rejection must leave dismissible Records");
      scene.modal.hide();
      expect(!scene.modal.root.visible, "Records must dismiss after result return or rejection");
    }
  }
}
void testExport() {
  for (const auto &result : {ReplayVideoExportResult{true, "output", "Exported"},
                             ReplayVideoExportResult{false, {}, "No Chart"},
                             ReplayVideoExportResult{false, {}, "Cancelled"}}) {
    MainMenuScene scene;
    expect(scene.beginReplayExport("Export", "Preparing", "Exporting"), "export entry must succeed");
    expect(scene.replayExportInProgress && scene.modal.operationInProgress(), "export must lock both owner/modal");
    scene.modal.hide();
    expect(scene.modal.root.visible, "export in progress must block dismissal");
    scene.pendingReplayExportResult = result;
    scene.applyReplayExportResult();
    expect(!scene.replayExportInProgress && !scene.willStart && scene.modal.canHide(), "export completion must release owner/modal");
    expect(!scene.modal.status.empty(), "export completion must publish status");
  }
}
int main() {
  testAutoPlay();
  testRecall();
  testExport();
  return failures == 0 ? 0 : 1;
}
