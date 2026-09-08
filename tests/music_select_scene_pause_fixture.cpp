#include <atomic>
#include <cassert>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <optional>
#include "REPOSITORY_ROOT/src/music_select/MusicSelectFolderStatusLoader.h"

namespace audio::diag {
void SelectAudioLog(const char *) {}
}

struct Preview {
  void reset() {}
  void silence() {}
  void resumeDefaultBgm() {}
};

struct ExternalUrlService {
  void close(int) {}
};

struct Worker {
  std::jthread thread;
  void stop() {
    thread.request_stop();
    if (thread.joinable()) thread.join();
  }
};

struct MusicSelectScene {
  std::atomic_bool launchCancelled_ = false;
  std::uint64_t launchGeneration_ = 0;
  std::jthread launchThread_;
  struct { std::atomic_bool appInBackground = false; } context;
  bool sceneActive_ = true;
  bool launching_ = false, failed_ = false;
  std::unique_ptr<MusicSelectFolderStatusLoader> folderStatusLoader_ =
      std::make_unique<MusicSelectFolderStatusLoader>();
  std::optional<std::uint64_t> folderStatusRowsRevision_ = 42;
  std::optional<std::chrono::steady_clock::time_point> folderStatusRetryAt_ =
      std::chrono::steady_clock::now();
  int reloads = 0, selections = 0;
  Worker *preloadWorker_ = nullptr;
  std::mutex preloadMutex_;
  std::unique_ptr<int> preloadedChart_;
  std::filesystem::path preloadedPath_;
  Preview previewController_;
  Preview *previewAudio_ = nullptr;
  int irExternalUrlGeneration_ = 0;
  ExternalUrlService *irExternalUrlService_ = nullptr;
  void stopInputListening() {}
  void cancelDirectoryLoad() {}
  void stopPreloadWorker();
  void onPause();
  void onResume();
  void hideDecideOverlay() {}
  void onApplicationBackgroundChanged(bool) {}
  void syncToolbar() {}
  void reloadLibrary() { ++reloads; }
  void configureSoundServices() {}
  void startInputListening() {}
  void selectedBarMoved() {
    ++selections;
    assert(folderStatusLoader_->request({MusicSelectBar{.id = {"current"}}}, "ALL", 1,
        [](const MusicSelectBar &, std::stop_token) {
          skin::MusicSelectBarFrame frame;
          frame.lamp = 3;
          return frame;
        }));
  }
};

SCENE_METHODS

int main() {
  MusicSelectScene scene;
  std::atomic_bool statsEntered = false, statsCancelled = false;
  scene.folderStatusLoader_->request({MusicSelectBar{.id = {"old"}}}, "ALL", 1,
      [&](const MusicSelectBar &, std::stop_token stop) {
        statsEntered = true;
        while (!stop.stop_requested()) std::this_thread::yield();
        statsCancelled = true;
        skin::MusicSelectBarFrame frame;
        frame.lamp = 99;
        return frame;
      });
  while (!statsEntered) std::this_thread::yield();
  Worker worker;
  scene.preloadWorker_ = &worker;
  std::atomic_bool entered = false;
  std::atomic_bool finished = false;
  worker.thread = std::jthread([&](std::stop_token stop) {
    entered.store(true);
    while (!stop.stop_requested()) std::this_thread::yield();
    std::lock_guard lock(scene.preloadMutex_);
    scene.preloadedChart_ = std::make_unique<int>(42);
    finished.store(true);
  });
  while (!entered.load()) std::this_thread::yield();
  scene.onPause();
  assert(finished.load() && "pause must join before handing off Jukebox");
  assert(!scene.preloadedChart_ && "publication must be cleared after joining");
  assert(!scene.folderStatusRowsRevision_ && !scene.folderStatusRetryAt_ &&
         "pause must reset folder statistics revision and retry state");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!statsCancelled && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  assert(statsCancelled && "pause must cancel active folder statistics");
  assert(scene.folderStatusLoader_->takeResults().empty());
  scene.onResume();
  assert(scene.sceneActive_ && scene.reloads == 1 && scene.selections == 1);
  std::vector<MusicSelectFolderStatusLoader::Result> results;
  while (results.empty() && std::chrono::steady_clock::now() < deadline)
    results = scene.folderStatusLoader_->takeResults();
  assert(results.size() == 1 && results.front().id.value == "current" &&
         results.front().frame.lamp == 3);
  scene.onPause();
  finished.store(false);
  worker.thread = std::jthread([&](std::stop_token) { finished.store(true); });
  scene.onPause();
  assert(finished.load() && "a resumed worker must be joined again");
}
