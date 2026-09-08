#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <string>

struct StartupTiming {
  static StartupTiming &instance() { static StartupTiming timing; return timing; }
  void mark(const char *) {}
};
struct Chart {};
struct ChartMetaRecord { struct Meta { std::string BmsPath; } meta; };
std::string fspath_to_utf8(const std::string &path) { return path; }
std::string fspath_to_path_t(const std::string &path) { return path; }
namespace play_options {
std::unique_ptr<Chart> parseChart(const ChartMetaRecord::Meta &, std::atomic_bool &,
                                const char *) { return std::make_unique<Chart>(); }
}
struct Worker { bool superseded(const std::string &) { return false; } };
struct Jukebox {
  struct Result { bool success; };
  bool success = true;
  Result loadChartPreservingDevice(Chart &, bool, std::atomic_bool &) { return {success}; }
};
struct Scene {
  struct Context { Jukebox jukebox; } context;
  Worker worker;
  Worker *preloadWorker_ = &worker;
  std::mutex preloadMutex_;
  std::string preloadedPath_ = "chart.bms";
  std::unique_ptr<Chart> preloadedChart_;
  void preload() {
    auto callback = PRELOAD_CALLBACK;
    std::atomic_bool cancelled = false;
    callback(ChartMetaRecord{{preloadedPath_}}, cancelled);
  }
};
int main() {
  Scene scene;
  scene.context.jukebox.success = false;
  scene.preload();
  assert(!scene.preloadedChart_ && "failed audio staging must not publish a reusable chart");
  scene.context.jukebox.success = true;
  scene.preload();
  assert(scene.preloadedChart_ && "successful audio staging remains reusable");
}
