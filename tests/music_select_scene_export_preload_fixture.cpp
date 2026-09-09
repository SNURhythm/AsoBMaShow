#include <atomic>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using path_t = std::string;
std::string fspath_to_path_t(std::string_view path) { return std::string(path); }
std::string fspath_to_utf8(std::string_view path) { return std::string(path); }
void SDL_Log(const char *, ...) {}

struct Metadata {
  std::string BmsPath = "selected-bga.bms";
  bool IsDP = false;
  int LnMode = 0;
};
struct ChartMetaRecord {
  Metadata meta;
  bool solidArchive = false;
  bool unavailable = false;
};
namespace bms_parser {
struct Chart { Metadata Meta; };
}

WORKER_DECLARATION
WORKER_METHODS

struct Settings {
  struct { struct { double masterVolume = 1, keysoundVolume = 1, bgmVolume = 1; } audio; } audioVideo;
  int skinPlayer2RandomOption = 0;
  int selectedPlaybackRatePercent = 100;
  int selectedPlaybackMode = 0;
  bool gameplayClubModeEnabled = false;
  bool inputKeysoundEnabled = true;
  int skinDoublePlayOption = 0;
  bool archiveChartPreviewEnabled = false;
};
namespace main_menu_profile {
struct Selections {
  std::string playOption = "OFF";
  std::string longNoteMode = "LN";
  int gaugeType = 0;
  int gaugeAutoShift = 0;
  int gaugeAutoShiftLowerBound = 0;
  int assistOption = 0;
  int ruleset = 0;
  int pacemakerTarget = 0;
  static Selections fromSettings(const Settings &) { return {}; }
};
}
bool preparationAvailable = true;
namespace play_options {
struct PlayOptionReplayInfo {
  std::string option;
  int seed = 0;
  std::string option2;
  int seed2 = 0;
};
std::unique_ptr<bms_parser::Chart> parseChart(
    const Metadata &meta, std::atomic_bool &, const char *) {
  if (!preparationAvailable) return nullptr;
  return std::make_unique<bms_parser::Chart>(bms_parser::Chart{meta});
}
PlayOptionReplayInfo applySelectedPlayOptions(bms_parser::Chart &, const std::string &) {
  return {};
}
bool applyPlayOptionModifier(bms_parser::Chart &, const std::string &,
                             std::nullopt_t, int, std::string &, int &, const char *) {
  return true;
}
}
namespace long_note_mode {
int valueFromId(const std::string &) { return 0; }
}
int normalizeChartLongNoteModeValue(int value) { return value; }
void applyEffectiveLongNoteModeToChart(bms_parser::Chart &, int) {}
void applyDoublePlayFlipToChart(bms_parser::Chart &) {
  assert(false && "export preload fixture does not enable double-play flip");
}
struct ModernChartResultRecord {};
struct ReplayData {};
namespace replay {
std::optional<std::string> beatorajaReplayOptionName(int) { return "OFF"; }
struct Loaded {
  std::unique_ptr<bms_parser::Chart> chart;
  std::unique_ptr<ReplayData> replayData = std::make_unique<ReplayData>();
  bool ready() const { return chart != nullptr; }
};
struct Consumer {
  Loaded load(const ModernChartResultRecord &, const std::string &path,
              std::atomic_bool &cancelled) {
    return {play_options::parseChart(Metadata{path}, cancelled, "replay")};
  }
};
Consumer makeRuntimeChartReplayConsumer(int) { return {}; }
}
namespace replay_autoplay {
struct Playback { int percent; int mode; };
ReplayData BuildReplayData(bms_parser::Chart &, int, int, Playback,
                           const std::string &, int, const std::string &, int,
                           int, bool, int, int) { return {}; }
}
struct ReplayVideoExportProgress { float fraction; std::string message; };
struct ReplayVideoExportOptions {
  std::function<void(const ReplayVideoExportProgress &)> progressCallback;
  std::stop_token stop;
  bool renderTouchPoints = true;
  bool renderReplayGhosts = true;
};
struct ReplayVideoExportResult { bool success; std::string message; };
struct Modal {
  bool exporting = false;
  bool progressVisible = false;
  std::string status;
  void resize(int, int) {}
  void update() {}
  void cancelAndWait() {}
  bool isVisible() const { return false; }
  void setExportInProgress(bool value) { exporting = value; }
  void showExportProgress(const char *, const char *) { progressVisible = true; }
  void returnToList(const std::string &message) {
    progressVisible = false;
    status = message;
  }
};
struct Repository {
  std::uint64_t revision = 0;
  std::uint64_t GetRevision() const { return revision; }
  std::uint64_t GetLibraryRevision() const { return revision; }
};
struct Context {
  struct {
    std::atomic_int stops = 0, loads = 0;
    void stop() { ++stops; }
    struct Result { bool success = true; };
    Result loadChart(bms_parser::Chart &, bool, std::atomic_bool &) { ++loads; return {}; }
  } jukebox;
  using AudioSettings = decltype(Settings{}.audioVideo.audio);
  std::vector<std::string> audioEvents;
  struct {
    std::vector<AudioSettings> applied;
    std::vector<std::string> *events;
    void apply(const AudioSettings &settings) {
      applied.push_back(settings);
      events->push_back("apply");
    }
  } audioDeviceManager{{}, &audioEvents};
  std::vector<AudioSettings> savedAudio;
  bool saveSettings() {
    savedAudio.push_back(settings.audioVideo.audio);
    audioEvents.push_back("save");
    return true;
  }
  Settings settings;
  int replayRepository = 0;
  Repository chartRepository;
  Repository scoreRepository;
  std::atomic_bool appInBackground = false;
  std::atomic_uint64_t irAccountEvidenceRevision = 0;
  struct SceneManager *sceneManager = nullptr;
  std::atomic_bool visualsLoaded = false;
  std::atomic_bool exportEntered = false;
  std::atomic_bool releaseExport = false;
  ReplayVideoExportResult exportResult{true, "Exported"};
  std::function<void()> checkHandoff;
};
struct ReplayVideoExporter {
  static ReplayVideoExportResult Export(Context &context, bms_parser::Chart *,
                                        const ReplayData &, ReplayVideoExportOptions) {
    context.checkHandoff();
    context.visualsLoaded = false;
    context.exportEntered = true;
    while (!context.releaseExport.load()) std::this_thread::yield();
    return context.exportResult;
  }
};
namespace skin {
enum class MusicSelectBarKind { Song, Hash, Folder, SameFolder, SearchWord, Grade, RandomCourse };
enum class MusicSelectSkinActionKind { Event, FloatWriter, StringWriter };
struct Action {
  MusicSelectSkinActionKind kind = MusicSelectSkinActionKind::Event;
  int selector = 15;
  double floatValue = 0;
  std::string stringValue;
};
}
std::optional<int> numericSelector(int value) { return value; }
std::string selectorName(int) { return {}; }
struct PublishedActions {
  std::vector<skin::Action> actions;
  auto takePublishedActions() { return std::exchange(actions, {}); }
};
struct MusicSelectBarId {
  std::string value;
  bool operator==(const MusicSelectBarId &) const = default;
};
struct Bar {
  skin::MusicSelectBarKind kind = skin::MusicSelectBarKind::Song;
  std::optional<ChartMetaRecord> chart = ChartMetaRecord{};
  MusicSelectBarId id{"song"};
  bool childrenLoaded = false;
};
using MusicSelectBar = Bar;
bool musicSelectIsSolidArchiveDirectory(const Bar &) { return false; }
bool musicSelectIsSolidArchiveAction(const Bar &) { return false; }
struct Bars {
  struct Rows {
    std::string error;
    const std::string &diagnostic() const { return error; }
  };
  std::shared_ptr<Rows> rowProvider;
  std::vector<Bar> directoryBars;
  void setSelectedPosition(float) {}
  std::vector<Bar> rows{Bar{}};
  std::size_t selectedIndex = 0;
  const Bars &readView() const { return *this; }
  std::size_t rowCount() const { return rows.size(); }
  const Bar &rowAt(std::size_t index) const { return rows.at(index); }
  bool select(const MusicSelectBarId &id) {
    rows.front().kind = id.value == "hash" ? skin::MusicSelectBarKind::Hash
                                         : skin::MusicSelectBarKind::Song;
    rows.front().id = id;
    return true;
  }
  bool open(const MusicSelectBarId &) {
    rows.front().kind = skin::MusicSelectBarKind::Song;
    rows.front().id = {"song"};
    return true;
  }
  void installFolderStatus(const MusicSelectBarId &, int) {}
};
namespace audio {
struct PlaybackRate { int percent; int mode; };
namespace diag { void SelectAudioLog(const std::string &) {} }
}
struct StartOptions {
  int startPosition;
  bool autoKeySound, autoPlay;
  int gaugeType, gaugeAutoShift, gaugeAutoShiftLowerBound;
  std::string playOption;
  int playOptionSeed;
  std::string playOption2;
  int playOption2Seed;
  bool doublePlayFlip;
  int longNoteMode, assistOption, pacemakerTarget;
  std::string tableName, tableLevel;
  bool practiceMode;
  audio::PlaybackRate playback;
  bool clubMode;
  void *returnScene;
  int ruleset;
};
struct GamePlayScene {
  std::unique_ptr<bms_parser::Chart> chart;
  GamePlayScene(Context &, std::unique_ptr<bms_parser::Chart> value, StartOptions)
      : chart(std::move(value)) {}
};
struct SceneManager {
  int launches = 0;
  std::function<void()> pause;
  std::unique_ptr<GamePlayScene> gameplay;
  void changeScene(std::unique_ptr<GamePlayScene> value, bool retained) {
    assert(retained);
    ++launches;
    pause();
    gameplay = std::move(value);
  }
};
auto musicSelectTableContextForLaunch(const Bars &) {
  struct Table { std::string name = "table", level = "12"; };
  return Table{};
}
struct StartupTiming {
  static StartupTiming &instance() { static StartupTiming timing; return timing; }
  void mark(const char *) {}
  void beginSession() {}
};
namespace rendering { int window_width = 1280, window_height = 720; }
namespace platform_open { bool openExternalUrl(const std::string &, std::string &) { return true; } }
struct View {
  void setViewportSize(int, int) {}
  void setSize(int, int) {}
  void resize(int, int) {}
  bool getVisible() { return false; }
};
struct ExternalUrl {
  struct Snapshot { int generation; bool finished; std::optional<std::string> url; };
  Snapshot snapshot() { return {}; }
  void close(int) {}
};
struct FolderStatus {
  struct Result { std::string error; MusicSelectBarId id; int frame; };
  std::vector<Result> takeResults() { return {}; }
  void cancel() {}
};
struct Preview {
  struct Request { std::optional<std::filesystem::path> path; };
  void observeSelection(int, int) {}
  std::optional<Request> update(int, bool) { return std::nullopt; }
  void switchTo(std::optional<std::filesystem::path>) {}
  void reset() {}
  void silence() {}
  void resumeDefaultBgm() {}
  void playDecide() {}
};
int previewSelection(const Bars &, bool) { return 0; }
struct MusicSelectScene {
  Modal *archiveUnzipModal_ = nullptr;
  bool selectorInputBlocked() const { return launching_; }
  void startArchiveUnzip(const ChartMetaRecord &) {}
  PublishedActions *skinSession_ = nullptr;
  Preview *systemSound_ = nullptr;
  int rankingOffset_ = 0;
  struct { int totalPlayers = 0, offset = 0; } ranking_;
  int eventsDispatched = 0;
  void executeEvent(const skin::Action &) { ++eventsDispatched; launchSelected(false, false); }
  void search(const std::string &) {}
  void launchCourse(const Bar &, bool) {}
  void showDecideOverlay(const ChartMetaRecord &) {}
  void hideDecideOverlay() {}
  void syncToolbar() {}
  void configureSoundServices() {}
  void startInputListening() {}
  void onApplicationBackgroundChanged(bool) {}
  std::mutex postedMutex_;
  std::vector<std::function<bool()>> posted_;
  void postDeferred(std::function<bool()> callback) {
    std::lock_guard lock(postedMutex_);
    posted_.push_back(std::move(callback));
  }
  void onResume();
  Context context;
  Bars bars_;
  bool launching_ = false;
  bool sceneActive_ = true, failed_ = false;
  std::uint64_t launchGeneration_ = 0;
  std::atomic_bool launchCancelled_ = false;
  std::jthread launchThread_;
  std::optional<int> folderStatusRowsRevision_;
  struct PendingPreloadLaunch { ChartMetaRecord record; bool autoplay, practice; };
  std::optional<PendingPreloadLaunch> pendingLaunch_;
  std::vector<MusicSelectBarId> restoreDirectories_;
  std::vector<MusicSelectBar> restoreDirectoryBars_;
  std::optional<MusicSelectBarId> restoreSelection_;
  int directoryPublications = 0, inputConsumptions = 0;
  bool hashDirectoryOpen = false;
  std::function<void()> logicalHandoff, actionHandoff;
  int irExternalUrlGeneration_ = 0;
  ExternalUrl *irExternalUrlService_ = nullptr;
  View *toolbar_ = nullptr, *searchOverlay_ = nullptr, *modalLayer_ = nullptr;
  View *modalOverlayPortal_ = nullptr, *playOptionsModal_ = nullptr, *tasksModal_ = nullptr;
  std::uint64_t irAccountEvidenceRevision_ = 0;
  std::optional<std::int64_t> startInputMicros_;
  FolderStatus *folderStatusLoader_ = nullptr;
  std::optional<std::chrono::steady_clock::time_point> folderStatusRetryAt_;
  Preview previewController_;
  Preview *previewAudio_ = nullptr;
  int songBarChangeMicros_ = 0;
  int elapsedMicros() { return 1000; }
  void refreshTasksModal() {}
  void applyRecordsExportProgress() {}
  void consumeLogicalInput() { ++inputConsumptions; if (logicalHandoff) logicalHandoff(); }
  void consumeActions() { ++inputConsumptions; if (actionHandoff) actionHandoff(); }
  void stopInputListening() {}
  void applyDirectoryLoads() { ++directoryPublications; }
  void requestFolderStatus(const Bars &) {}
  void updateRanking() {}
  void launchSelected(bool, bool) {}
  void cancelDirectoryLoad() { restoreDirectories_.clear(); }
  bool openSameFolder(bool) { return false; }
  void requestDirectoryLoad(const Bar &) { assert(false); }
  bool loadDirectoryChildren(const Bar &bar) {
    assert(bar.kind == skin::MusicSelectBarKind::Hash);
    ++directoryPublications;
    return true;
  }
  void continueDirectoryRestore();
  void tryCompletePendingPreloadLaunch();
  void update(float);
  void onPause();
  ChartPreloadWorker *preloadWorker_ = nullptr;
  std::mutex preloadMutex_;
  std::unique_ptr<bms_parser::Chart> preloadedChart_;
  std::string preloadedPath_;
  Modal *recordsModal_ = nullptr;
  std::atomic_bool recordsExportInProgress_ = false;
  std::jthread recordsExportThread_;
  std::mutex recordsExportProgressMutex_;
  struct PendingRecordsExportProgress { float fraction; std::string message; };
  std::optional<PendingRecordsExportProgress> pendingRecordsExportProgress_;
  std::mutex recordsExportResultMutex_;
  std::optional<ReplayVideoExportResult> pendingRecordsExportResult_;
  std::uint64_t libraryRevision_ = 0;
  std::uint64_t scoreRevision_ = 0;
  void reloadLibrary() {
    libraryRevision_ = context.chartRepository.GetLibraryRevision();
    scoreRevision_ = context.scoreRepository.GetRevision();
    if (hashDirectoryOpen) {
      restoreDirectories_ = {MusicSelectBarId{"hash"}};
      restoreSelection_ = MusicSelectBarId{"song"};
    }
    if (!restoreDirectories_.empty()) continueDirectoryRestore();
  }
  void selectedBarMoved() { startPreloadForSelection(); }
  void refreshRepositoryRevisions();
  void startPreloadForSelection();
  void stopPreloadWorker();
  bool reusePreloadedChart(const ChartMetaRecord &, bms_parser::Chart *&,
                          play_options::PlayOptionReplayInfo &, int &);
  void launchChartReplayExport(const ChartMetaRecord &, const ModernChartResultRecord &,
                               ReplayVideoExportOptions);
  void launchAutoPlayExport(const ChartMetaRecord &, ReplayVideoExportOptions);
  void applyRecordsExportResult();
};

SCENE_METHODS

int failures = 0;
std::string caseName;
void expect(bool condition, const char *message) {
  if (!condition) {
    ++failures;
    std::cerr << caseName << ": " << message << '\n';
  }
}

void runCase(bool autoplay, bool active, int outcome, bool withModal) {
  caseName = std::string(autoplay ? "autoplay" : "replay") +
             (active ? "/active/" : "/idle/") + std::to_string(outcome) +
             (withModal ? "/modal" : "/no-modal");
  MusicSelectScene scene;
  Modal modal;
  if (withModal) scene.recordsModal_ = &modal;
  ChartPreloadWorker worker(std::chrono::milliseconds(0));
  scene.preloadWorker_ = &worker;
  const ChartMetaRecord record;
  std::atomic_bool preloadEntered = false;
  std::atomic_bool preloadFinished = false;
  std::atomic_bool holdPreload = active;
  worker.configure([&](const ChartMetaRecord &requested, std::atomic_bool &cancelled) {
    preloadEntered = true;
    while (holdPreload.load() && !cancelled.load()) std::this_thread::yield();
    {
      std::lock_guard lock(scene.preloadMutex_);
      scene.context.visualsLoaded = true;
      scene.preloadedChart_ = std::make_unique<bms_parser::Chart>(
          bms_parser::Chart{requested.meta});
    }
    preloadFinished = true;
  });
  scene.startPreloadForSelection();
  while (!preloadEntered.load()) std::this_thread::yield();
  if (!active) {
    while (!preloadFinished.load()) std::this_thread::yield();
    while (worker.isRequesting(record.meta.BmsPath)) std::this_thread::yield();
  }
  scene.context.checkHandoff = [&] {
    expect(preloadFinished.load(), "export must join the active preload before taking visuals");
    std::lock_guard lock(scene.preloadMutex_);
    expect(!scene.preloadedChart_ && scene.preloadedPath_.empty(),
           "export must invalidate reusable publication after joining");
  };
  preparationAvailable = outcome != 3;
  if (outcome == 1) scene.context.exportResult = {false, "Encoding unavailable"};
  if (outcome == 2) scene.context.exportResult = {false, "No Chart"};
  if (autoplay) scene.launchAutoPlayExport(record, {});
  else scene.launchChartReplayExport(record, {}, {});
  if (outcome != 3) {
    while (!scene.context.exportEntered.load()) std::this_thread::yield();
  } else {
    scene.recordsExportThread_.join();
  }
  expect(scene.recordsExportInProgress_.load(), "export lock must last until result delivery");
  if (withModal) expect(modal.exporting && modal.progressVisible, "export must show progress");
  ++scene.context.scoreRepository.revision;
  scene.refreshRepositoryRevisions();
  ++scene.context.chartRepository.revision;
  scene.refreshRepositoryRevisions();
  scene.startPreloadForSelection();
  {
    std::lock_guard lock(scene.preloadMutex_);
    expect(scene.preloadedPath_.empty(), "revision refresh must not queue preload during export");
  }
  scene.context.releaseExport = true;
  if (scene.recordsExportThread_.joinable()) scene.recordsExportThread_.join();
  scene.applyRecordsExportResult();
  expect(!scene.recordsExportInProgress_.load(), "all outcomes must release the export lock");
  if (withModal) {
    expect(!modal.exporting && !modal.progressVisible && !modal.status.empty(),
           "all outcomes must restore records UI with a result");
  }
  bms_parser::Chart *cached = nullptr;
  play_options::PlayOptionReplayInfo playInfo;
  int lnMode = 0;
  expect(!scene.reusePreloadedChart(record, cached, playInfo, lnMode),
         "next same-selection play must not skip loading BGA after export");
  delete cached;
  worker.stop();
  scene.stopPreloadWorker();
  holdPreload = false;
  preloadFinished = false;
  preparationAvailable = true;
  scene.context.visualsLoaded = false;
  scene.startPreloadForSelection();
  while (!preloadFinished.load()) std::this_thread::yield();
  expect(scene.context.visualsLoaded.load(), "preloading must recover and restore BGA after export");
  cached = nullptr;
  expect(scene.reusePreloadedChart(record, cached, playInfo, lnMode),
         "fresh post-export load must become reusable");
  delete cached;
  scene.stopPreloadWorker();
}

int main() {
  for (bool autoplay : {false, true}) {
    for (bool active : {false, true}) {
      for (int outcome = 0; outcome < 4; ++outcome) {
        for (bool withModal : {false, true}) runCase(autoplay, active, outcome, withModal);
      }
    }
  }
  return failures == 0 ? 0 : 1;
}
