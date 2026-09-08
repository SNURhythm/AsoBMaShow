#include "REPOSITORY_ROOT/src/audio/AudioMix.h"

#include <atomic>
#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

const auto uiThread = std::this_thread::get_id();
void assertUi() { assert(std::this_thread::get_id() == uiThread); }
struct Gate {
  std::atomic_bool entered = false, released = false, observedCancellation = false;
  bool block = false;
  void run(std::atomic_bool &cancelled) {
    assert(std::this_thread::get_id() != uiThread && "course staging must leave the UI thread");
    entered = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (block && !released && !cancelled && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    assert(!block || released || cancelled);
    observedCancellation = cancelled.load();
  }
  void wait() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(entered);
  }
  void reset() { entered = released = observedCancellation = false; block = false; }
};
Gate parseGate, audioGate;
bool parseSuccess = true;

namespace bms_parser {
struct ChartMeta { std::string BmsPath; };
struct Chart {
  inline static std::atomic_int alive = 0;
  ChartMeta Meta;
  int constraints = 0, longNoteMode = 0;
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
struct Selections { static Selections fromSettings(const Settings &) { assertUi(); return {}; } };
}
namespace replay {
std::optional<std::string> beatorajaReplayOptionName(int value) { assertUi(); return value == 1 ? "MIRROR" : "NORMAL"; }
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
  int courseId = 0;
  std::string courseKey, courseName, courseGroupName, constraintJson;
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
  assertUi();
  session->courseId = request.courseId;
  session->courseKey = request.courseKey;
  session->courseName = request.courseName;
  session->courseGroupName = request.courseGroupName;
  session->constraintJson = request.constraintJson;
  session->constraints = 11;
  session->longNoteMode = 2;
  session->gaugeType = 3;
  session->gaugeProfile = 4;
  session->gaugeAutoShift = 5;
  session->gaugeAutoShiftLowerBound = 6;
  session->ruleset = 7;
  session->rulesetDescriptor = 8;
  session->assistOption = "ASSIST";
  session->autoKeySound = !request.inputKeysoundEnabled;
  session->doublePlayFlip = request.doublePlayFlip;
  session->requestedPlayOption = "RANDOM";
  session->requestedPlayOption2 = request.player2PlayOption;
  session->records = std::move(request.records);
  return session;
}
namespace play_options {
struct PlayOptionReplayInfo {
  std::optional<std::string> option, option2;
  std::optional<long long> seed, seed2;
};
std::unique_ptr<bms_parser::Chart> parseChart(
    const std::string &path, std::atomic_bool &cancelled, const char *) {
  parseGate.run(cancelled);
  if (!parseSuccess) return nullptr;
  return std::make_unique<bms_parser::Chart>(path);
}
PlayOptionReplayInfo applySelectedPlayOptions(
    bms_parser::Chart &, const std::string &first, const std::string &second) {
  return {first, second, 123, 456};
}
}
void applyCourseConstraintsToChart(bms_parser::Chart &chart, int value) { chart.constraints = value; }
void applyEffectiveLongNoteModeToChart(bms_parser::Chart &chart, int value) { chart.longNoteMode = value; }
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
  void changeScene(std::unique_ptr<GamePlayScene> scene, bool retained) {
    assertUi();
    assert(retained);
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
    audioGate.run(cancelled);
    ++loads;
    if (cancel) cancelled = true;
    return {.success = success, .diagnostic = success ? "" : "Backend unavailable"};
  }
};
struct Bars {
  std::vector<MusicSelectBar> children;
  int revision = 0;
  int readView() const { assertUi(); return revision; }
  const std::vector<MusicSelectBar> &childrenOf(int) const { assertUi(); return children; }
};
struct TableContext { std::string name, level; };
TableContext musicSelectTableContextForLaunch(int revision) {
  assertUi();
  return {"Table" + std::to_string(revision), "12"};
}
namespace audio::diag { void SelectAudioLog(const char *) {} }
struct Preview {
  bool silenced = false;
  int resumes = 0;
  void reset() { assertUi(); }
  void silence() { assertUi(); silenced = true; }
  void resumeDefaultBgm() { assertUi(); silenced = false; ++resumes; }
};
struct ExternalUrl { void close(int) {} };
struct FolderStatusLoader { void cancel() {} };
struct MusicSelectScene {
  SceneManager manager;
  struct {
    Settings settings;
    Jukebox jukebox;
    SceneManager *sceneManager;
    std::atomic_bool appInBackground = false;
  } context;
  bool launching_ = false;
  bool sceneActive_ = true, failed_ = false, overlayVisible = false;
  std::atomic_bool launchCancelled_ = false;
  std::uint64_t launchGeneration_ = 0;
  std::jthread launchThread_;
  Preview previewController_;
  std::unique_ptr<Preview> previewAudio_ = std::make_unique<Preview>();
  std::unique_ptr<FolderStatusLoader> folderStatusLoader_;
  std::unique_ptr<int> directoryLoader_;
  std::optional<int> folderStatusRowsRevision_, folderStatusRetryAt_;
  int irExternalUrlGeneration_ = 0;
  ExternalUrl *irExternalUrlService_ = nullptr;
  std::mutex postedMutex;
  std::vector<std::function<bool()>> posted;
  bool preloadStopped = false;
  Bars bars_;
  MusicSelectScene() { context.sceneManager = &manager; }
  ~MusicSelectScene() { cleanupScene(); }
  void stopPreloadWorker() { assertUi(); preloadStopped = true; }
  void showDecideOverlay(const ChartMetaRecord &) { assertUi(); overlayVisible = true; }
  void hideDecideOverlay() { assertUi(); overlayVisible = false; }
  void stopInputListening() { assertUi(); }
  void startInputListening() { assertUi(); }
  void cancelDirectoryLoad() { assertUi(); }
  void syncToolbar() { assertUi(); }
  void reloadLibrary() { assertUi(); }
  void configureSoundServices() { assertUi(); }
  void selectedBarMoved() { assertUi(); }
  void onApplicationBackgroundChanged(bool) {}
  void onPause();
  void onResume();
  void cleanupScene() { CLEANUP_LAUNCH }
  void postDeferred(std::function<bool()> callback) {
    std::lock_guard lock(postedMutex);
    posted.push_back(std::move(callback));
  }
  void drain() {
    assertUi();
    std::vector<std::function<bool()>> callbacks;
    {
      std::lock_guard lock(postedMutex);
      callbacks.swap(posted);
    }
    for (auto &callback : callbacks) callback();
  }
  void finish() {
    if (launchThread_.joinable()) launchThread_.join();
    drain();
  }
  void launchCourse(const MusicSelectBar &bar, bool autoplay);
  void launchDirectoryAutoplay(const MusicSelectBar &directory);
};

SCENE_METHODS

void testCourseAudioFailure(bool folderAutoplay) {
  MusicSelectScene scene;
  MusicSelectBar course;
  course.title = "Course";
  course.courseCharts = {{{"first.bms"}}, {{"second.bms"}}};
  MusicSelectBar directory;
  directory.title = "Folder";
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
  scene.finish();
  assert(scene.manager.transitions == 0 &&
         "failed course/folder audio staging must not enter gameplay");
  assert(!scene.launching_ && "failed staging must allow another selector launch");
  assert(bms_parser::Chart::alive == 0 && "failed staging must release the chart");
  assert(scene.context.jukebox.loads == 1 && scene.preloadStopped);

  scene.context.jukebox.success = true;
  launch();
  scene.finish();
  assert(scene.manager.transitions == 1 && scene.context.jukebox.loads == 2);
  assert(scene.manager.gameplay->chart->Meta.BmsPath == "first.bms");
  assert(scene.manager.gameplay->options.autoPlay == folderAutoplay);
  assert(scene.manager.gameplay->options.courseSession->autoPlay == folderAutoplay);
  assert(scene.manager.gameplay->options.courseSession->records.size() == 2);
  assert(scene.manager.gameplay->options.courseSession->courseName ==
         (folderAutoplay ? "Folder" : "Course"));
  assert(scene.manager.gameplay->options.returnScene == &scene);
}

void testCancelledCourseAudio() {
  MusicSelectScene scene;
  MusicSelectBar course;
  course.courseCharts = {{{"first.bms"}}};
  scene.context.jukebox.success = true;
  scene.context.jukebox.cancel = true;
  scene.launchCourse(course, false);
  scene.finish();
  assert(scene.manager.transitions == 0 && !scene.launching_);
  assert(bms_parser::Chart::alive == 0);
}

void resetGates() {
  parseGate.reset();
  audioGate.reset();
  parseSuccess = true;
}

void launchForFixture(MusicSelectScene &scene, const MusicSelectBar &course,
                      bool autoplay, bool folderAutoplay) {
  if (!folderAutoplay) {
    scene.launchCourse(course, autoplay);
    return;
  }
  scene.bars_.children.clear();
  for (const auto &record : course.courseCharts) {
    MusicSelectBar child;
    child.kind = skin::MusicSelectBarKind::Song;
    child.chart = record;
    scene.bars_.children.push_back(child);
  }
  MusicSelectBar directory;
  directory.title = "Folder";
  directory.kind = skin::MusicSelectBarKind::Folder;
  scene.launchDirectoryAutoplay(directory);
}

void testAsyncCourseLifecycle(bool folderAutoplay = false) {
  for (bool duringAudio : {false, true}) {
    for (bool cleanup : {false, true}) {
      resetGates();
      MusicSelectScene scene;
      MusicSelectBar course;
      course.courseCharts = {{{"first.bms"}}};
      auto &gate = duringAudio ? audioGate : parseGate;
      gate.block = true;
      scene.context.jukebox.success = true;
      launchForFixture(scene, course, false, folderAutoplay);
      gate.wait();
      assert(scene.launching_ && scene.overlayVisible && scene.previewAudio_->silenced);
      scene.drain();
      assert(scene.manager.transitions == 0 && "UI can drain while the worker is gated");
      if (cleanup) scene.cleanupScene();
      else scene.onPause();
      assert(gate.observedCancellation && !scene.launchThread_.joinable());
      scene.drain();
      assert(scene.manager.transitions == 0 && bms_parser::Chart::alive == 0);
      assert(scene.previewAudio_->resumes == 0);
      if (!cleanup) {
        scene.onResume();
        gate.block = false;
        launchForFixture(scene, course, false, folderAutoplay);
        scene.finish();
        assert(scene.manager.transitions == 1);
      }
    }
  }
  for (bool oldSuccess : {false, true}) {
    resetGates();
    MusicSelectScene scene;
    MusicSelectBar course;
    course.courseCharts = {{{"old.bms"}}};
    scene.context.jukebox.success = oldSuccess;
    launchForFixture(scene, course, false, folderAutoplay);
    scene.launchThread_.join();
    scene.onPause();
    scene.onResume();
    course.courseCharts = {{{"new.bms"}}};
    scene.context.jukebox.success = true;
    audioGate.reset();
    audioGate.block = true;
    launchForFixture(scene, course, true, folderAutoplay);
    audioGate.wait();
    scene.drain();
    assert(scene.manager.transitions == 0 && scene.launching_ && scene.overlayVisible &&
           "old deferred success/failure must not consume or reset the resumed launch");
    audioGate.released = true;
    scene.finish();
    assert(scene.manager.transitions == 1);
    assert(scene.manager.gameplay->chart->Meta.BmsPath == "new.bms");
  }
  for (bool cleanup : {false, true}) {
    resetGates();
    MusicSelectScene scene;
    scene.context.jukebox.success = true;
    MusicSelectBar course;
    course.courseCharts = {{{"queued.bms"}}};
    launchForFixture(scene, course, false, folderAutoplay);
    scene.launchThread_.join();
    if (cleanup) scene.cleanupScene();
    else scene.failed_ = true;
    scene.drain();
    assert(scene.manager.transitions == 0 && bms_parser::Chart::alive == 0);
  }
  {
    resetGates();
    MusicSelectScene scene;
    scene.context.jukebox.success = true;
    MusicSelectBar course;
    course.courseCharts = {{{"destroyed.bms"}}};
    launchForFixture(scene, course, false, folderAutoplay);
    scene.launchThread_.join();
    assert(bms_parser::Chart::alive == 1);
  }
  assert(bms_parser::Chart::alive == 0 && "teardown must release undrained completion ownership");
}

void testAsyncCourseOptionsAndParseRetry() {
  resetGates();
  MusicSelectScene scene;
  MusicSelectBar course;
  course.courseId = 12;
  course.courseKey = "key";
  course.title = "title";
  course.courseGroupName = "group";
  course.courseConstraintJson = "constraints";
  course.courseCharts = {{{"first.bms"}}, {{"second.bms"}}};
  scene.context.settings = {1, 1, false, true};
  parseSuccess = false;
  scene.launchCourse(course, false);
  scene.finish();
  assert(!scene.launching_ && !scene.overlayVisible && scene.context.jukebox.loads == 0);
  assert(scene.previewAudio_->resumes == 1 && bms_parser::Chart::alive == 0);
  parseSuccess = true;
  parseGate.reset();
  parseGate.block = true;
  scene.context.jukebox.success = true;
  scene.launchCourse(course, false);
  parseGate.wait();
  scene.context.settings = {};
  scene.bars_.revision = 99;
  scene.drain();
  assert(scene.manager.transitions == 0);
  parseGate.released = true;
  scene.launchThread_.join();
  assert(scene.manager.transitions == 0 && scene.launching_ && scene.overlayVisible);
  scene.drain();
  assert(scene.manager.transitions == 1);
  const auto &options = scene.manager.gameplay->options;
  const auto &session = *options.courseSession;
  assert(options.startPosition == 0 && options.autoKeySound && !options.autoPlay);
  assert(options.gaugeType == 3 && options.gaugeProfile == 4 &&
         options.gaugeAutoShift == 5 && options.gaugeAutoShiftLowerBound == 6);
  assert(options.playOption == "RANDOM" && options.playOption2 == "MIRROR" &&
         options.playOptionSeed == 123 && options.playOption2Seed == 456);
  assert(session.playOption == options.playOption && session.playOption2 == options.playOption2 &&
         session.playOptionSeed == options.playOptionSeed && session.playOption2Seed == options.playOption2Seed);
  assert(options.doublePlayFlip && options.longNoteMode == 2 && options.assistOption == "ASSIST");
  assert(options.tableName == "Table0" && options.tableLevel == "12" && options.clubMode);
  assert(options.playback == course_rules::kRequiredPlaybackRate && options.courseConstraints == 11);
  assert(options.ruleset == 7 && options.requiredRulesetDescriptor == 8 && options.ownsChart);
  assert(options.returnScene == &scene && session.records.size() == 2);
  assert(session.records[1].meta.BmsPath == "second.bms");
  assert(session.courseId == 12 && session.courseKey == "key" && session.courseName == "group title" &&
         session.courseGroupName == "group" && session.constraintJson == "constraints");
  assert(scene.manager.gameplay->chart->constraints == 11 && scene.manager.gameplay->chart->longNoteMode == 2);
}

void testCourseFailureWhileApplicationBackgrounded() {
  resetGates();
  MusicSelectScene scene;
  MusicSelectBar course;
  course.courseCharts = {{{"background.bms"}}};
  audioGate.block = true;
  scene.launchCourse(course, false);
  audioGate.wait();
  scene.context.appInBackground = true;
  audioGate.released = true;
  scene.finish();
  assert(!scene.launching_ && !scene.overlayVisible && scene.manager.transitions == 0);
  assert(scene.previewAudio_->silenced && scene.previewAudio_->resumes == 0 &&
         "deferred failure must not restart selector BGM in the background");
  scene.context.appInBackground = false;
  scene.onResume();
  scene.context.jukebox.success = true;
  scene.launchCourse(course, false);
  scene.finish();
  assert(scene.manager.transitions == 1);
}

int main() {
  SCENE_TEST;
}
