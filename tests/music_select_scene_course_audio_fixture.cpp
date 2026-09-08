#include "REPOSITORY_ROOT/src/audio/AudioMix.h"

#include <atomic>
#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bms_parser {
struct ChartMeta { std::string BmsPath; };
struct Chart {
  inline static int alive = 0;
  ChartMeta Meta;
  explicit Chart(std::string path) : Meta{std::move(path)} { ++alive; }
  ~Chart() { --alive; }
};
}
struct ChartMetaRecord { bms_parser::ChartMeta meta; };
namespace skin { enum class MusicSelectBarKind { Song, Folder, Grade }; }
struct MusicSelectBar {
  int id = 1;
  skin::MusicSelectBarKind kind = skin::MusicSelectBarKind::Grade;
  int courseId = 0;
  std::string courseKey, title, courseGroupName, courseConstraintJson;
  std::vector<ChartMetaRecord> courseCharts;
  struct { bool exists = true; } presentation;
  std::optional<ChartMetaRecord> chart;
};
struct Settings {
  int skinPlayer2RandomOption = 0, skinDoublePlayOption = 0;
  bool inputKeysoundEnabled = true, gameplayClubModeEnabled = false;
};
namespace main_menu_profile {
struct Selections { static Selections fromSettings(const Settings &) { return {}; } };
}
namespace replay {
std::optional<std::string> beatorajaReplayOptionName(int) { return "NORMAL"; }
}
struct CourseGameplaySessionRequest {
  int courseId = 0;
  std::string courseKey, courseName, courseGroupName, constraintJson;
  std::vector<ChartMetaRecord> records;
  main_menu_profile::Selections selections;
  std::string player2PlayOption;
  bool doublePlayFlip = false, inputKeysoundEnabled = true;
};
struct CoursePlaySession {
  std::vector<ChartMetaRecord> records;
  bool autoPlay = false, autoKeySound = false, doublePlayFlip = false;
  int constraints = 0, longNoteMode = 0;
  int gaugeType = 0, gaugeProfile = 0, gaugeAutoShift = 0, gaugeAutoShiftLowerBound = 0;
  int ruleset = 0, rulesetDescriptor = 0;
  std::string requestedPlayOption = "NORMAL", requestedPlayOption2 = "NORMAL";
  std::string assistOption;
  std::optional<std::string> playOption, playOption2;
  std::optional<long long> playOptionSeed, playOption2Seed;
  const bms_parser::ChartMeta *currentMeta() const {
    return records.empty() ? nullptr : &records.front().meta;
  }
};
std::shared_ptr<CoursePlaySession> buildCourseGameplaySession(
    CourseGameplaySessionRequest request) {
  auto session = std::make_shared<CoursePlaySession>();
  session->records = std::move(request.records);
  return session;
}
namespace play_options {
struct PlayOptionReplayInfo {
  std::optional<std::string> option, option2;
  std::optional<long long> seed, seed2;
};
std::unique_ptr<bms_parser::Chart> parseChart(
    const std::string &path, std::atomic_bool &, const char *) {
  return std::make_unique<bms_parser::Chart>(path);
}
PlayOptionReplayInfo applySelectedPlayOptions(
    bms_parser::Chart &, const std::string &, const std::string &) { return {}; }
}
void applyCourseConstraintsToChart(bms_parser::Chart &, int) {}
void applyEffectiveLongNoteModeToChart(bms_parser::Chart &, int) {}
namespace course_rules { constexpr int kRequiredPlaybackRate = 100; }
struct StartOptions {
  int startPosition;
  bool autoKeySound, autoPlay;
  int gaugeType, gaugeProfile, gaugeAutoShift, gaugeAutoShiftLowerBound;
  std::optional<std::string> playOption;
  std::optional<long long> playOptionSeed;
  std::optional<std::string> playOption2;
  std::optional<long long> playOption2Seed;
  bool doublePlayFlip;
  int longNoteMode;
  std::string assistOption, tableName, tableLevel;
  int playback;
  bool clubMode;
  std::shared_ptr<CoursePlaySession> courseSession;
  int courseConstraints, ruleset, requiredRulesetDescriptor;
  bool ownsChart;
  void *returnScene;
};
struct GamePlayScene {
  std::unique_ptr<bms_parser::Chart> chart;
  StartOptions options;
  GamePlayScene(auto &, std::unique_ptr<bms_parser::Chart> value, StartOptions start)
      : chart(std::move(value)), options(std::move(start)) {}
};
struct SceneManager {
  int transitions = 0;
  std::unique_ptr<GamePlayScene> gameplay;
  void changeScene(std::unique_ptr<GamePlayScene> scene, bool) {
    ++transitions;
    gameplay = std::move(scene);
  }
};
struct Jukebox {
  bool success = false;
  bool cancel = false;
  int loads = 0;
  void stop() {}
  audio::playback::BackendOperationResult loadChart(
      bms_parser::Chart &, bool, std::atomic_bool &cancelled) {
    ++loads;
    cancelled = cancel;
    return {.success = success, .diagnostic = success ? "" : "Backend unavailable"};
  }
};
struct Bars {
  std::vector<MusicSelectBar> children;
  int readView() const { return 0; }
  const std::vector<MusicSelectBar> &childrenOf(int) const { return children; }
};
struct TableContext { std::string name, level; };
TableContext musicSelectTableContextForLaunch(int) { return {}; }
struct MusicSelectScene {
  SceneManager manager;
  struct { Settings settings; Jukebox jukebox; SceneManager *sceneManager; } context;
  bool launching_ = false;
  bool preloadStopped = false;
  Bars bars_;
  MusicSelectScene() { context.sceneManager = &manager; }
  void stopPreloadWorker() { preloadStopped = true; }
  void launchCourse(const MusicSelectBar &bar, bool autoplay);
  void launchDirectoryAutoplay(const MusicSelectBar &directory);
};

SCENE_METHODS

void testCourseAudioFailure(bool folderAutoplay) {
  MusicSelectScene scene;
  MusicSelectBar course;
  course.courseCharts = {{{"first.bms"}}, {{"second.bms"}}};
  MusicSelectBar directory;
  directory.kind = skin::MusicSelectBarKind::Folder;
  for (const auto &record : course.courseCharts) {
    MusicSelectBar child;
    child.kind = skin::MusicSelectBarKind::Song;
    child.chart = record;
    scene.bars_.children.push_back(child);
  }
  const auto launch = [&] {
    if (folderAutoplay) scene.launchDirectoryAutoplay(directory);
    else scene.launchCourse(course, false);
  };
  launch();
  assert(scene.manager.transitions == 0 &&
         "failed course/folder audio staging must not enter gameplay");
  assert(!scene.launching_ && "failed staging must allow another selector launch");
  assert(bms_parser::Chart::alive == 0 && "failed staging must release the chart");
  assert(scene.context.jukebox.loads == 1 && scene.preloadStopped);

  scene.context.jukebox.success = true;
  launch();
  assert(scene.manager.transitions == 1 && scene.context.jukebox.loads == 2);
  assert(scene.manager.gameplay->chart->Meta.BmsPath == "first.bms");
  assert(scene.manager.gameplay->options.autoPlay == folderAutoplay);
  assert(scene.manager.gameplay->options.courseSession->autoPlay == folderAutoplay);
  assert(scene.manager.gameplay->options.courseSession->records.size() == 2);
  assert(scene.manager.gameplay->options.returnScene == &scene);
}

void testCancelledCourseAudio() {
  MusicSelectScene scene;
  MusicSelectBar course;
  course.courseCharts = {{{"first.bms"}}};
  scene.context.jukebox.success = true;
  scene.context.jukebox.cancel = true;
  scene.launchCourse(course, false);
  assert(scene.manager.transitions == 0 && !scene.launching_);
  assert(bms_parser::Chart::alive == 0);
}

int main() {
  SCENE_TEST;
}
