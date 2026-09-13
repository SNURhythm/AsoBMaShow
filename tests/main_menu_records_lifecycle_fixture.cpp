#include "REPOSITORY_ROOT/src/replay/ReplayExportJob.h"
#include "REPOSITORY_ROOT/src/scene/ReplayRecordTask.h"
#include "REPOSITORY_ROOT/src/scene/FindBmsTask.h"
#include "REPOSITORY_ROOT/tests/support/AllocationFailure.h"
#include <atomic>
#include <cassert>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <vector>

void SDL_Log(const char *, ...) {}
std::string fspath_to_utf8(const std::filesystem::path &path) { return path.string(); }
namespace ir { std::string sanitizeDiagnostic(const std::string &value) { return value; } }
namespace replay_records {
std::string diagnosticOr(const std::string &value, const char *fallback) {
  return value.empty() ? fallback : value;
}
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
struct ResultRemoteOptions { void *returnScene = nullptr; };
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
struct PreviewWorker {
  std::function<void()> onStop;
  int deferredReleases = 0;
  void cancel() {}
  void cancelAndReleaseWhenIdle() { ++deferredReleases; }
  void stop() { if (onStop) onStop(); }
};

struct Recycler {
  int selectedIndex = -1;
  int itemCount = 0;
  int size() { return itemCount; }
  ChartMetaRecord get(int) { return {}; }
  std::function<void(const ChartMetaRecord &, int)> onSelected;
};
struct Callbacks {
  std::function<void(const ChartMetaRecord &)> watchAutoPlay;
  std::function<void(const ChartMetaRecord &, const ModernChartResultRecord &)> recallModernChart;
  std::function<void(const ModernCourseResultRecord &, bool)> recallModernCourse;
  std::function<void(const IrRemoteRecordId &, const std::string &)> recallRemote;
};
struct FindBmsLifetime {
  std::atomic_bool workerFinished = false;
  std::atomic_bool dependenciesAlive = true;
};
struct FindBmsDependencies {
  FindBmsLifetime *lifetime = nullptr;
  ~FindBmsDependencies() {
    if (lifetime == nullptr) return;
    assert(lifetime->workerFinished.load());
    lifetime->dependenciesAlive = false;
  }
};
struct MainMenuScene {
  void onApplicationBackgroundChanged(bool) {}
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
    std::atomic_bool appInBackground = false;
  } context;
  ProfileSelections profileSelections;
  ReplayRecordsModal modal;
  ReplayRecordsModal *recordsModal_ = &modal;
  PreviewWorker *previewWorker_ = nullptr;
  // Preserve the production ordering: Find BMS worker precedes its state.
  FindBmsTask findBmsTask;
  FindBmsDependencies findBmsDependencies;
  std::atomic_bool willStart = false;
  replay::ReplayExportJob replayExportJob_;
  ReplayRecordTask replayLoadTask_;
  bool replayResultRecallInProgress = false, replayIrUploadInProgress = false;
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
  ~MainMenuScene();
  void defer(std::function<bool()> callback, int, bool) { deferred.push_back(std::move(callback)); }
  void drain() {
    auto callbacks = std::move(deferred);
    deferred.clear();
    for (auto &callback : callbacks) callback();
  }
  bool prepareAutoPlayChartForRecord(const ChartMetaRecord &, std::unique_ptr<bms_parser::Chart> &chart,
                                    play_options::PlayOptionReplayInfo &, std::atomic_bool &, const ProfileSelections &, int) {
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
  int selectedChartRandomInfoForPath(const std::filesystem::path &) const { return 0; }
  bool finishReplayLoadFailure(const char *, std::string message, const char *fallback) {
    resetReplayWatchLoadingUi();
    modal.setStatus(message.empty() ? fallback : message);
    return true;
  }
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
  void stopReplayAndPreviewWork();
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
    while (scene.replayLoadTask_.active()) { scene.applyReplayLoadCompletion(); std::this_thread::yield(); }
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
      PreviewWorker preview;
      MainMenuScene scene;
      scene.previewWorker_ = &preview;
      scene.preparationFails = outcome == 1;
      scene.context.replayRepository.available = outcome != 1;
      remoteSelectionMatches = outcome != 2;
      const auto callbacks = scene.callbacks();
      if (path == 0) callbacks.recallModernChart({}, {});
      if (path == 1) callbacks.recallModernCourse({}, true);
      if (path == 2) callbacks.recallRemote({}, "remote");
      expect(preview.deferredReleases == (path < 2 ? 1 : 0),
             "local result recall must defer preview release through its owner");
      expect(scene.replayResultRecallInProgress && scene.modal.operationInProgress(), "recall entry must lock owner/modal");
      scene.modal.hide();
      expect(scene.modal.root.visible, "recall must block dismissal until completion");
      if (path < 2) {
        while (!scene.preparationEntered) std::this_thread::yield();
        if (outcome == 2) scene.stopReplayLoadWorker();
        else {
          scene.releasePreparation = true;
          while (scene.replayLoadTask_.active()) {
            scene.applyReplayLoadCompletion();
            std::this_thread::yield();
          }
        }
      } else {
        while (scene.replayLoadTask_.active()) { scene.applyReplayLoadCompletion(); std::this_thread::yield(); }
      }
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
    expect(scene.replayExportJob_.inProgress() && scene.modal.operationInProgress(), "export must lock both owner/modal");
    scene.modal.hide();
    expect(scene.modal.root.visible, "export in progress must block dismissal");
    scene.replayExportJob_.start({}, [result](const auto &, auto &) { return result; });
    while (scene.replayExportJob_.inProgress()) {
      scene.applyReplayExportResult();
      std::this_thread::yield();
    }
    expect(!scene.replayExportJob_.inProgress() && !scene.willStart && scene.modal.canHide(), "export completion must release owner/modal");
    expect(!scene.modal.status.empty(), "export completion must publish status");
  }
}
void testExportStartupFailureRestoresRecordsAndPreview() {
  std::atomic_bool ran = false;
  int previewRestarts = 0;
  Recycler recycler;
  recycler.selectedIndex = 0;
  recycler.itemCount = 1;
  recycler.onSelected = [&](const auto &, int) { ++previewRestarts; };
  View status;
  MainMenuScene scene;
  scene.recyclerView = &recycler;
  scene.replayStatusText = &status;
  expect(scene.beginReplayExport("Export", "Preparing", "Exporting"),
         "startup failure fixture reserves the export and UI");
  replay::ReplayExportJob::Work work = [&](const auto &, auto &) {
    ran = true;
    return ReplayVideoExportResult{};
  };
  ReplayVideoExportOptions options;
  bool threw = false;
  try {
    const test_support::FailNextAllocation failure;
    scene.replayExportJob_.start(std::move(options), std::move(work));
  } catch (const std::bad_alloc &) {
    threw = true;
  }
  expect(!threw, "startup failure must reach the Main Menu result consumer");
  if (threw) return;
  expect(!ran && !scene.replayExportJob_.hasWorker() && scene.willStart &&
             scene.modal.operationInProgress(),
         "failed startup retains UI ownership until result consumption");
  scene.applyReplayExportResult();
  expect(!scene.replayExportJob_.inProgress() && !scene.willStart &&
             scene.modal.canHide() && !scene.modal.status.empty() && !status.text.empty(),
         "startup failure clears Main Menu busy state and publishes its diagnostic");
  expect(previewRestarts == 1, "startup failure restores preview for the selected chart");
  scene.applyReplayExportResult();
  expect(previewRestarts == 1, "startup failure restores preview only once");
}
void testDestructionStopsPreparationBeforePreviewDependencies() {
  std::atomic_bool loadStopped = false, exportStopped = false;
  std::atomic_bool loadStarted = false, exportStarted = false;
  bool previewStopped = false;
  PreviewWorker preview;
  preview.onStop = [&] {
    expect(loadStopped && exportStopped, "preview stop must follow both preparation owners");
    previewStopped = true;
  };
  {
    MainMenuScene scene;
    scene.previewWorker_ = &preview;
    scene.replayLoadTask_.start([&](std::shared_ptr<std::atomic_bool> cancelled) {
      loadStarted = true;
      while (!cancelled->load()) std::this_thread::yield();
      loadStopped = true;
    });
    expect(scene.replayExportJob_.tryBegin(), "export worker reservation must succeed");
    scene.replayExportJob_.start({}, [&](const auto &, std::atomic_bool &cancelled) {
      exportStarted = true;
      while (!cancelled.load()) std::this_thread::yield();
      exportStopped = true;
      return ReplayVideoExportResult{false, {}, "Cancelled"};
    });
    // Exercise teardown of in-flight work. A task cancelled before dispatch
    // legitimately never enters its callback.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!loadStarted || !exportStarted) && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    expect(loadStarted && exportStarted, "both preparation workers must reach the teardown barrier");
  }
  expect(previewStopped && loadStopped && exportStopped,
         "destruction must join playback work while its scene dependencies are alive");
}
void testDestructionStopsFindBmsBeforeStatusDependencies() {
  using namespace std::chrono_literals;
  FindBmsLifetime lifetime;
  std::promise<void> entered, stopped, release;
  auto released = release.get_future().share();
  auto scene = std::make_unique<MainMenuScene>();
  scene->findBmsDependencies.lifetime = &lifetime;
  scene->findBmsTask.start(
      [&](std::atomic_bool &cancelled, BmsSearchDownloadProgressCallback) {
        entered.set_value();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!cancelled.load() &&
               std::chrono::steady_clock::now() < deadline) {
          std::this_thread::yield();
        }
        assert(cancelled.load());
        stopped.set_value();
        assert(released.wait_for(5s) == std::future_status::ready);
        // Pending artifact transactions can finish after cancellation.
        assert(lifetime.dependenciesAlive.load());
        lifetime.workerFinished = true;
        return BmsSearchResult{};
      });
  assert(entered.get_future().wait_for(5s) == std::future_status::ready);
  auto destroyed = std::async(std::launch::async, [&] { scene.reset(); });
  assert(stopped.get_future().wait_for(5s) == std::future_status::ready);
  assert(lifetime.dependenciesAlive.load());
  assert(destroyed.wait_for(50ms) == std::future_status::timeout);
  release.set_value();
  assert(destroyed.wait_for(5s) == std::future_status::ready);
  destroyed.get();
  assert(lifetime.workerFinished && !lifetime.dependenciesAlive);
}
int main() {
  testExportStartupFailureRestoresRecordsAndPreview();
  testAutoPlay();
  testRecall();
  testExport();
  testDestructionStopsPreparationBeforePreviewDependencies();
  testDestructionStopsFindBmsBeforeStatusDependencies();
  return failures == 0 ? 0 : 1;
}
