#include "REPOSITORY_ROOT/src/audio/AudioMix.h"
#include <atomic>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

void SDL_Log(const char *, ...) {}
namespace bms_parser {
struct Chart {
  inline static int alive = 0;
  Chart() { ++alive; }
  ~Chart() { --alive; }
};
}
struct ChartMetaRecord {
  struct { std::filesystem::path BmsPath = "chart.bms"; } meta;
  bool unavailable = false, solidArchive = false;
};
struct ModernChartResultRecord { struct { int score = 51; } result; };
struct ModernCourseResultRecord { struct { std::vector<int> stages{1, 2}; } result; };
namespace skin { enum class MusicSelectBarKind { Song, Grade }; }
struct MusicSelectBar {
  skin::MusicSelectBarKind kind = skin::MusicSelectBarKind::Song;
  std::optional<ChartMetaRecord> chart = ChartMetaRecord{};
  std::vector<ChartMetaRecord> courseCharts{ChartMetaRecord{}, ChartMetaRecord{}};
};
struct MusicSelectBarManagerReadView {
  std::vector<MusicSelectBar> rows{MusicSelectBar{}};
  unsigned selectedIndex = 0;
  unsigned rowCount() const { return rows.size(); }
  const MusicSelectBar &rowAt(unsigned index) const { return rows.at(index); }
};
struct Bars {
  MusicSelectBarManagerReadView snapshot;
  auto readView() const { return snapshot; }
};
namespace long_note_mode { int valueFromId(int mode) { return mode; } }
struct Slot { std::filesystem::path relativePath = "slot.json"; };
auto musicSelectCourseReplaySlotPaths(const MusicSelectBar &, int) {
  return std::optional<std::vector<Slot>>(std::vector<Slot>(4));
}
auto musicSelectChartReplaySlotPaths(const ChartMetaRecord &, int) {
  return std::optional<std::vector<Slot>>(std::vector<Slot>(4));
}
auto musicSelectCourseReplayResultId(const auto &, const Slot &) { return std::optional<int>(7); }
auto musicSelectChartReplayResultId(const auto &, const Slot &) { return std::optional<int>(7); }
enum class ModernReplayFileInventoryStatus { Loaded };
enum class ModernCourseResultReadStatus { Loaded };
enum class ModernChartResultReadStatus { Loaded };
struct ReplayRepository {
  auto GetResolvedProfileRoot() { return std::filesystem::path("."); }
  auto ListModernReplayFileReferences() {
    struct Inventory {
      ModernReplayFileInventoryStatus status = ModernReplayFileInventoryStatus::Loaded;
      std::vector<int> entries;
      std::string diagnostic;
    };
    return Inventory{};
  }
  auto LoadModernCourseResult(int) {
    struct Stored {
      ModernCourseResultReadStatus status = ModernCourseResultReadStatus::Loaded;
      std::optional<ModernCourseResultRecord> record = ModernCourseResultRecord{};
      std::string diagnostic;
    };
    return Stored{};
  }
  auto LoadModernChartResult(int) {
    struct Stored {
      ModernChartResultReadStatus status = ModernChartResultReadStatus::Loaded;
      std::optional<ModernChartResultRecord> record = ModernChartResultRecord{};
      std::string diagnostic;
    };
    return Stored{};
  }
};
struct ReplayData { int initialGaugeType = 8, gaugeAutoShift = 9; };
struct CourseSession {
  int currentIndex = 0;
  bool applied = false;
  std::shared_ptr<ReplayData> data = std::make_shared<ReplayData>();
  bool hasCourseReplayStage(int index) { return index == 0; }
  auto currentCourseReplayStageReplay() { return data; }
  void applyReplayStagePlayOptions(const ReplayData &) { applied = true; }
  auto takePreparedCourseChart(int) { return std::make_unique<bms_parser::Chart>(); }
};
namespace replay {
struct Loaded {
  std::unique_ptr<bms_parser::Chart> chart = std::make_unique<bms_parser::Chart>();
  std::shared_ptr<ReplayData> replayData = std::make_shared<ReplayData>();
  std::string diagnostic;
  bool ready() const { return true; }
};
struct Consumer {
  Loaded load(const auto &, const auto &, std::atomic_bool &) { return {}; }
};
Consumer makeRuntimeChartReplayConsumer(ReplayRepository &) { return {}; }
Consumer makeRuntimeCourseReplayConsumer(ReplayRepository &) { return {}; }
enum class CourseReplayLaunchMode { Watch };
auto makeCourseReplayLaunchSession(Loaded, CourseReplayLaunchMode) {
  return std::make_shared<CourseSession>();
}
}
namespace main_menu_profile {
struct Selections {
  int pacemakerTarget = 43;
  static Selections fromSettings(const auto &) { return {}; }
};
}
namespace pacemaker { constexpr int kTargetOff = -1; }
struct StartOptions {
  int startPosition = 0;
  bool autoKeySound = false, autoPlay = false;
  int gaugeType = 0, gaugeAutoShift = 0;
  std::shared_ptr<ReplayData> replayData;
  int pacemakerTarget = 0;
  std::string tableName, tableLevel;
  bool touchVisualizationEnabled = false, replayGhostRenderingEnabled = false;
  void *returnScene = nullptr;
  int provenance = 0;
  bool ghost = false;
  std::shared_ptr<CourseSession> courseSession;
};
StartOptions makeCourseReplayStageStartOptions(std::shared_ptr<CourseSession> session,
                                               std::shared_ptr<ReplayData> data) {
  return {.gaugeType = data->initialGaugeType, .gaugeAutoShift = data->gaugeAutoShift,
          .replayData = data, .provenance = 77, .courseSession = session};
}
void applyReplayProvenanceToStartOptions(StartOptions &options, const ReplayData &) {
  options.provenance = 77;
}
struct Playback { int percent, mode; };
StartOptions musicSelectGhostBattleOptions(std::shared_ptr<ReplayData> data, int,
    main_menu_profile::Selections, bool, Playback, void *scene) {
  return {.replayData = data, .returnScene = scene, .provenance = 78, .ghost = true};
}
struct TableContext { std::string name = "Table", level = "12"; };
TableContext musicSelectTableContextForLaunch(const MusicSelectBarManagerReadView &) { return {}; }
struct GamePlayScene {
  std::unique_ptr<bms_parser::Chart> chart;
  StartOptions options;
  GamePlayScene(auto &, std::unique_ptr<bms_parser::Chart> value, StartOptions start)
      : chart(std::move(value)), options(std::move(start)) {}
};
struct SceneManager {
  int transitions = 0;
  std::unique_ptr<GamePlayScene> gameplay;
  void changeScene(std::unique_ptr<GamePlayScene> scene, bool retained) {
    assert(retained);
    ++transitions;
    gameplay = std::move(scene);
  }
};
struct RecordsModal {
  bool loading = false, visible = true;
  std::string status;
  void setLoadInProgress(bool value) { loading = value; }
  void setStatus(const std::string &value) { status = value; }
  bool renderTouchPoints() { return true; }
  bool renderReplayGhosts() { return false; }
  void hide() { visible = false; }
};
struct Jukebox {
  bool success = false, cancel = false;
  int loads = 0;
  void stop() {}
  audio::playback::BackendOperationResult loadChart(
      bms_parser::Chart &, bool, std::atomic_bool &cancelled) {
    ++loads;
    cancelled = cancel;
    return {.success = success, .diagnostic = "Audio unavailable"};
  }
};
struct MusicSelectScene {
  SceneManager manager;
  struct {
    struct {
      int selectedLnMode = 1, selectedPlaybackRatePercent = 100, selectedPlaybackMode = 0;
      bool inputKeysoundEnabled = true;
    } settings;
    ReplayRepository replayRepository;
    Jukebox jukebox;
    SceneManager *sceneManager;
  } context;
  bool launching_ = false;
  int preloadStops = 0;
  Bars bars_;
  RecordsModal modal;
  RecordsModal *recordsModal_ = &modal;
  MusicSelectScene() { context.sceneManager = &manager; }
  void stopPreloadWorker() { ++preloadStops; }
  void launchCourseReplay(const MusicSelectBar &, int, const MusicSelectBarManagerReadView &);
  void launchSelectedReplay(int);
  void launchChartReplay(const ChartMetaRecord &, const ModernChartResultRecord &, bool);
};

SCENE_METHODS

void testReplayAudio(int path) {
  std::ofstream("slot.json") << "fixture";
  MusicSelectScene scene;
  if (path == 1) scene.bars_.snapshot.rows.front().kind = skin::MusicSelectBarKind::Grade;
  const auto launch = [&] {
    if (path >= 2) scene.launchChartReplay({}, {}, path == 3);
    else scene.launchSelectedReplay(0);
  };
  for (bool cancel : {false, true}) {
    scene.context.jukebox.success = cancel;
    scene.context.jukebox.cancel = cancel;
    launch();
    assert(scene.manager.transitions == 0 && "failed or cancelled replay audio must not launch");
    assert(!scene.launching_ && "replay audio failure must permit retry");
    assert(bms_parser::Chart::alive == 0 && "rejected replay must release prepared chart");
    assert(!scene.modal.loading && scene.modal.visible);
  }
  scene.context.jukebox.success = true;
  scene.context.jukebox.cancel = false;
  launch();
  assert(scene.manager.transitions == 1 && scene.preloadStops == 3);
  const auto &options = scene.manager.gameplay->options;
  assert(options.returnScene == &scene && options.replayData);
  assert(options.provenance == (path == 3 ? 78 : 77));
  assert(options.ghost == (path == 3));
  if (path < 2) {
    assert(options.tableName == "Table" && options.tableLevel == "12");
    assert(options.gaugeType == 8 && options.gaugeAutoShift == 9);
    if (path == 0) assert(options.pacemakerTarget == 43);
    else assert(options.courseSession && options.courseSession->applied);
  } else {
    assert(!scene.modal.loading && !scene.modal.visible);
    if (path == 2) {
      assert(options.touchVisualizationEnabled && !options.replayGhostRenderingEnabled);
      assert(options.pacemakerTarget == pacemaker::kTargetOff);
    }
  }
}

int main() { SCENE_TEST; }
