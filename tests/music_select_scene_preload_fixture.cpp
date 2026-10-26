#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
#include <string>
#include <filesystem>
#include <chrono>
#include <stop_token>
#include <thread>
#include "REPOSITORY_ROOT/src/RAII.h"

namespace audio {
struct DecodedAssetCache {
  static constexpr std::size_t kUnlimitedRetainedBytes = 0;
  explicit DecodedAssetCache(std::size_t) {}
};
}

struct Chart { struct { std::filesystem::path BmsPath; } Meta; };
struct ChartMetaRecord { struct Meta { std::string BmsPath; } meta; };
std::string fspath_to_utf8(const std::string &path) { return path; }
std::string fspath_to_utf8(const std::filesystem::path &path) { return path.string(); }
void SDL_Log(const char *, ...) {}
std::string fspath_to_path_t(const std::string &path) { return path; }
namespace play_options {
std::unique_ptr<Chart> parseChart(const ChartMetaRecord::Meta &meta, std::atomic_bool &,
                                const char *) {
  auto chart = std::make_unique<Chart>();
  chart->Meta.BmsPath = meta.BmsPath;
  return chart;
}
}
struct Worker {
  std::atomic_bool requesting{false};
  std::atomic_int checks{0};
  bool superseded(const std::string &) { return false; }
  bool isRequesting(const std::string &) { ++checks; return requesting; }
};
namespace audio {
constexpr int kPreviewSampleRate = 44100;
long long FindChartPreviewHighlightMicros(const Chart &) { return 40'000'000; }
struct PagedPreviewSource {
  struct Gains { float bgm, keysound; } gains;
  std::uint64_t first;
  std::shared_ptr<DecodedAssetCache> cache;
  static std::shared_ptr<PagedPreviewSource> Create(const Chart &, std::shared_ptr<DecodedAssetCache> cache,
                                                   std::uint64_t first, Gains gains) {
    return std::make_shared<PagedPreviewSource>(PagedPreviewSource{gains, first, std::move(cache)});
  }
};
}
struct Player {
  struct Request { std::string deviceId; unsigned sampleRate, bufferFrames; } request;
  std::shared_ptr<audio::PagedPreviewSource> playing;
  bool faded = false;
  int starts = 0;
  bool Start(std::shared_ptr<audio::PagedPreviewSource> source, std::string &, bool fade) {
    playing = std::move(source); faded = fade; ++starts; return true;
  }
  void SetMonoAudio(bool) {}
  void SetRequest(Request value) { request = std::move(value); }
};
struct Jukebox {
  struct Result { bool success; };
  bool success = true;
  std::shared_ptr<audio::DecodedAssetCache> cache;
  void setDecodedAssetCache(std::shared_ptr<audio::DecodedAssetCache> value) { cache = std::move(value); }
  Result loadChartPreservingDevice(Chart &, bool, std::atomic_bool &) { assert(cache); return {success}; }
};
struct Scene {
  struct Context {
    Jukebox jukebox;
    struct Settings {
      struct AudioVideo {
        struct Audio {
          bool monoAudio = false;
          float masterVolume = 0.5f, bgmVolume = 0.4f, keysoundVolume = 0.6f;
          std::string outputDeviceId = "chosen";
          unsigned requestedSampleRate = 48000, requestedBufferFrames = 256;
        } audio;
      } audioVideo;
    } settings;
  } context;
  Player chartPreviewPlayer_;
  bool playChartPreview(const std::filesystem::path &path, bool highlight,
                        const std::shared_ptr<std::atomic_bool> &cancelled, std::stop_token stop)
    CHART_PREVIEW_BODY
  Worker worker;
  Worker *preloadWorker_ = &worker;
  std::mutex preloadMutex_;
  std::string preloadedPath_ = "chart.bms";
  std::unique_ptr<Chart> preloadedChart_;
  std::shared_ptr<audio::DecodedAssetCache> preloadedPreviewCache_;
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
  assert(scene.preloadedPreviewCache_ && !scene.context.jukebox.cache &&
         "native PCM stays reusable by preview without leaving a jukebox cache override");
  auto cancel = std::make_shared<std::atomic_bool>(false);
  assert(scene.playChartPreview("chart.bms", true, cancel, {}));
  assert(scene.chartPreviewPlayer_.playing->cache == scene.preloadedPreviewCache_ &&
         scene.chartPreviewPlayer_.playing->first == 40 * 44100 && scene.chartPreviewPlayer_.faded);
  assert(scene.chartPreviewPlayer_.playing->gains.bgm == 0.2f &&
         scene.chartPreviewPlayer_.playing->gains.keysound == 0.3f &&
         scene.chartPreviewPlayer_.request.deviceId == "chosen");
  assert(scene.playChartPreview("chart.bms", false, cancel, {}));
  assert(scene.chartPreviewPlayer_.playing->first == 0 && !scene.chartPreviewPlayer_.faded);
  assert(!scene.playChartPreview("other.bms", true, cancel, {}));
  scene.preloadedChart_.reset();
  scene.worker.requesting = true;
  const auto starts = scene.chartPreviewPlayer_.starts;
  scene.worker.checks = 0;
  std::thread waiting([&] { assert(!scene.playChartPreview("chart.bms", true, cancel, {})); });
  while (!scene.worker.checks) std::this_thread::yield();
  cancel->store(true);
  waiting.join();
  assert(scene.chartPreviewPlayer_.starts == starts && "cancelled preload wait must never start audio");
}
