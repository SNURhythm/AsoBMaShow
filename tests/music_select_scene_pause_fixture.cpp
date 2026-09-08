#include <atomic>
#include <cassert>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace audio::diag {
void SelectAudioLog(const char *) {}
}

struct Preview {
  void reset() {}
  void silence() {}
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
  bool sceneActive_ = true;
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
};

SCENE_METHODS

int main() {
  MusicSelectScene scene;
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
  scene.onPause();
  finished.store(false);
  worker.thread = std::jthread([&](std::stop_token) { finished.store(true); });
  scene.onPause();
  assert(finished.load() && "a resumed worker must be joined again");
}
