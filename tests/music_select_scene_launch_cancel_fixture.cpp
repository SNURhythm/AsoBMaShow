#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

struct LoadGate {
  std::atomic_bool entered = false;
  std::atomic_bool observedCancellation = false;
  bool block = false;
  void run(std::atomic_bool &cancelled) {
    entered = true;
    if (!block) return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!cancelled.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    observedCancellation = cancelled.load();
  }
  void wait() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    assert(entered && "launch worker must reach the controlled load boundary");
  }
};
LoadGate parseGate;
LoadGate audioGate;
const auto uiThread = std::this_thread::get_id();

namespace bms_parser {
struct Chart { struct { bool IsDP = true; } Meta; };
}
struct ChartMetaRecord { struct { int LnMode = 0; } meta; };
namespace play_options {
struct PlayOptionReplayInfo { int option = 0, seed = 0, option2 = 0, seed2 = 0; };
std::unique_ptr<bms_parser::Chart> parseChart(
    const auto &, std::atomic_bool &cancelled, const char *) {
  parseGate.run(cancelled);
  return std::make_unique<bms_parser::Chart>();
}
bool applyPlayOptionModifier(bms_parser::Chart &, const auto &option, std::nullopt_t,
                             int side, int &, int &, const char *) {
  if constexpr (std::is_same_v<std::decay_t<decltype(option)>, std::string>) {
    assert(side == 1 && option == "NORMAL");
  }
  return true;
}
}
namespace replay {
std::optional<std::string> beatorajaReplayOptionName(int value) {
  assert(std::this_thread::get_id() == uiThread && "capture player-two settings before the worker");
  return value == 0 ? "NORMAL" : "MIRROR";
}
}
namespace long_note_mode { int valueFromId(int value) { return value; } }
int normalizeChartLongNoteModeValue(int value) { return value; }
void applyEffectiveLongNoteModeToChart(bms_parser::Chart &, int) {}
void applyDoublePlayFlipToChart(bms_parser::Chart &) {
  assert(false && "launch cancellation fixture does not enable double-play flip");
}
struct StartupTiming {
  static StartupTiming &instance() { static StartupTiming timing; return timing; }
  void mark(const char *) {}
};
struct StartOptions {
  int startPosition, autoKeySound, autoPlay, gaugeType, gaugeAutoShift;
  int gaugeAutoShiftLowerBound, playOption, playOptionSeed, playOption2;
  int playOption2Seed, doublePlayFlip, longNoteMode, assistOption, pacemakerTarget;
  std::string tableName, tableLevel;
  bool practiceMode;
  int playback;
  bool clubMode;
  void *returnScene;
  int ruleset;
};
struct GamePlayScene {
  GamePlayScene(auto &, std::unique_ptr<bms_parser::Chart>, StartOptions) {}
};
struct SceneManager {
  std::thread::id uiThread = std::this_thread::get_id();
  int launches = 0;
  void changeScene(std::unique_ptr<GamePlayScene>, bool) {
    assert(std::this_thread::get_id() == uiThread);
    ++launches;
  }
};
struct Jukebox {
  struct Result { bool success; };
  bool success = true;
  void stop() {}
  Result loadChart(bms_parser::Chart &, bool, std::atomic_bool &cancelled) {
    audioGate.run(cancelled);
    return {success};
  }
};
struct PreviewAudio {
  int resumes = 0;
  void resumeDefaultBgm() { ++resumes; }
};
struct MusicSelectScene {
  struct UnzipModal {};
  std::unique_ptr<UnzipModal> archiveUnzipModal_;
  struct Context {
    struct { int skinPlayer2RandomOption = 0; } settings;
    Jukebox jukebox;
    SceneManager *sceneManager;
  } context;
  SceneManager manager;
  std::atomic_bool launchCancelled_ = false;
  std::uint64_t launchGeneration_ = 0;
  std::jthread launchThread_;
  bool sceneActive_ = true;
  bool failed_ = false;
  bool launching_ = true;
  bool overlayVisible = true;
  std::unique_ptr<PreviewAudio> previewAudio_ = std::make_unique<PreviewAudio>();
  std::unique_ptr<int> directoryLoader_, folderStatusLoader_;
  std::mutex postedMutex;
  std::vector<std::function<bool()>> posted;
  MusicSelectScene() { context.sceneManager = &manager; }
  void cancelDirectoryLoad() {}
  void hideDecideOverlay() { overlayVisible = false; }
  void postDeferred(std::function<bool()> callback) {
    std::lock_guard lock(postedMutex);
    posted.push_back(std::move(callback));
  }
  void drain() {
    std::vector<std::function<bool()>> callbacks;
    {
      std::lock_guard lock(postedMutex);
      callbacks.swap(posted);
    }
    for (auto &callback : callbacks) callback();
  }
  void launch() {
    ChartMetaRecord record;
    struct {
      int playOption = 0, longNoteMode = 0, gaugeType = 0, gaugeAutoShift = 0;
      int gaugeAutoShiftLowerBound = 0, assistOption = 0, pacemakerTarget = 0;
      int ruleset = 0;
    } selections;
    bool autoKeySound = false, doublePlayFlip = false;
    int playback = 100;
    bool clubMode = false, practice = false, autoplay = false;
    struct { std::string name, level; } tableContext;
    LAUNCH_WORKER
  }
  void cleanupScene() {
    CLEANUP_LAUNCH
  }
};

void testCancellation(bool duringAudio) {
  parseGate.block = !duringAudio;
  audioGate.block = duringAudio;
  parseGate.entered = false;
  audioGate.entered = false;
  MusicSelectScene scene;
  scene.launch();
  auto &gate = duringAudio ? audioGate : parseGate;
  gate.wait();
  scene.cleanupScene();
  assert(gate.observedCancellation &&
         "cleanup cancellation must reach the in-progress parser/audio operation");
  assert(!scene.launchThread_.joinable());
  scene.drain();
  assert(scene.manager.launches == 0 && "cleanup must never launch gameplay");
  assert(scene.previewAudio_->resumes == 0 && "cleanup must not revive selector audio");
}

void testQueuedCompletionAfterCleanup() {
  parseGate.block = audioGate.block = false;
  MusicSelectScene scene;
  scene.launch();
  scene.launchThread_.join();
  scene.cleanupScene();
  scene.drain();
  assert(scene.manager.launches == 0 && "queued success must not launch after cleanup");
}

void testQueuedCompletionAfterError() {
  MusicSelectScene scene;
  scene.launch();
  scene.launchThread_.join();
  scene.failed_ = true;
  scene.drain();
  assert(scene.manager.launches == 0 && "queued success must not replace the error view");
}

void testActiveCompletionAndFailure() {
  MusicSelectScene scene;
  scene.launch();
  scene.launchThread_.join();
  assert(scene.manager.launches == 0 && "worker must defer scene changes to the UI");
  scene.drain();
  assert(scene.manager.launches == 1);
  scene.launching_ = true;
  scene.context.jukebox.success = false;
  scene.launch();
  scene.launchThread_.join();
  scene.drain();
  assert(scene.manager.launches == 1 && !scene.launching_ && !scene.overlayVisible);
  assert(scene.previewAudio_->resumes == 1);
}

void testQueuedCompletionAfterResume(bool oldSuccess) {
  MusicSelectScene scene;
  scene.context.jukebox.success = oldSuccess;
  scene.launch();
  scene.launchThread_.join();
  scene.cleanupScene();
  scene.sceneActive_ = true;
  scene.launching_ = true;
  scene.context.jukebox.success = true;
  audioGate.entered = false;
  audioGate.block = true;
  scene.launch();
  audioGate.wait();
  scene.context.settings.skinPlayer2RandomOption = 1;
  scene.drain();
  assert(scene.manager.launches == 0 && scene.launching_ && scene.overlayVisible &&
         scene.previewAudio_->resumes == 0 &&
         "old normal launch completion must not consume or reset a resumed launch");
  scene.cleanupScene();
  scene.drain();
  audioGate.block = false;
}

int main() {
  testCancellation(false);
  testCancellation(true);
  testQueuedCompletionAfterCleanup();
  testQueuedCompletionAfterError();
  testActiveCompletionAndFailure();
  testQueuedCompletionAfterResume(false);
  testQueuedCompletionAfterResume(true);
}
