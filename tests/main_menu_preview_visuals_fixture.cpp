#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Run the production worker and clock adapter with device/parser boundaries
// replaced, so no native audio device or renderer is needed for this wiring test.
struct ChartMetaRecord { struct { std::filesystem::path BmsPath; } meta; };
namespace bms_parser { struct Chart {}; }
namespace audio {
struct DecodedAssetCache {
  static constexpr std::size_t kUnlimitedRetainedBytes = std::numeric_limits<std::size_t>::max();
  explicit DecodedAssetCache(std::filesystem::path, std::size_t limit) : retainedLimit(limit) {}
  std::size_t retainedLimit;
};
}
namespace audio {
inline constexpr std::uint64_t kPreviewSampleRate = 44100;
struct PagedPreviewSource {
  static inline bool failConstruction = false;
  struct Gains { float bgm, keysound; };
  static inline Gains lastGains{};
  static inline std::shared_ptr<DecodedAssetCache> lastCache;
  std::uint64_t first = 0;
  static std::shared_ptr<PagedPreviewSource> Create(const bms_parser::Chart &,
      std::shared_ptr<DecodedAssetCache> cache, std::uint64_t first, Gains gains) {
    if (failConstruction) throw std::runtime_error("duration exceeds frame limit");
    lastGains = gains;
    lastCache = std::move(cache);
    auto result = std::make_shared<PagedPreviewSource>();
    result->first = first;
    return result;
  }
  void Cancel() {}
};
}
namespace play_options {
std::unique_ptr<bms_parser::Chart> parseChart(std::filesystem::path,
    std::atomic_bool &, const char *) { return std::make_unique<bms_parser::Chart>(); }
}
namespace archive_file { void appendDebugLogLine(std::string) {} }
std::string fspath_to_utf8(const std::filesystem::path &path) { return path.string(); }
template <typename... Args> void SDL_Log(const char *, Args...) {}
struct Player {
  struct State {
    bool playing = false, buffering = false, finished = false;
    std::uint64_t positionFrames = 0;
    std::string error;
  } state;
  struct Request { std::string deviceId; unsigned sampleRate, bufferFrames; };
  std::vector<std::string> *events = nullptr;
  bool fail = false;
  bool Start(std::shared_ptr<audio::PagedPreviewSource> source, std::string &) {
    events->push_back("audio-start");
    state = {.playing = !fail, .positionFrames = source->first};
    return !fail;
  }
  void Stop() { state = {}; events->push_back("audio-stop"); }
  void SetRequest(Request) {}
  void SetGain(float) {}
  State GetState() const { return state; }
};
struct Visuals {
  std::vector<std::string> events;
  bool loaded = true;
  std::function<void(std::atomic_bool &)> duringLoad;
  std::vector<long long> positions;
  void stop() { events.push_back("jukebox-stop"); }
  void unloadVisuals() { loaded = false; events.push_back("visual-unload"); }
  void loadVisuals(bms_parser::Chart &, std::atomic_bool &cancelled) {
    events.push_back("visual-load");
    loaded = true;
    if (duringLoad) duringLoad(cancelled);
  }
  void seekVisualsToSongTime(long long micros) { positions.push_back(micros); }
};
struct Worker { bool superseded(std::string) const { return false; } };
struct Scene {
  struct Context {
    Visuals jukebox;
    struct Settings {
      struct AudioVideo {
        struct Audio {
          float masterVolume = 1, bgmVolume = 1, keysoundVolume = 1;
          std::string outputDeviceId;
          unsigned requestedSampleRate = 48000, requestedBufferFrames = 128;
        } audio;
      } audioVideo;
    } settings;
  } context;
  Player previewPlayer_;
  std::shared_ptr<audio::DecodedAssetCache> previewDecodedAssets_;
  std::uint64_t previewStartFrame_ = 0;
  bool previewVisualsLoaded_ = false;
  std::mutex previewJukeboxLoadMutex;
  std::unique_ptr<Worker> previewWorker_ = std::make_unique<Worker>();
  std::atomic_bool willStart = false;
  std::shared_ptr<bms_parser::Chart> selectedChart;
  bool mediaReady = false;
  Scene() { previewPlayer_.events = &context.jukebox.events; }
  void setSelectedChart(std::shared_ptr<bms_parser::Chart> chart, bool ready) {
    selectedChart = std::move(chart); mediaReady = ready;
  }
  void clearSelectedChart() { selectedChart.reset(); }
  void process(const ChartMetaRecord &request, std::atomic_bool &cancelled)
    PREVIEW_PROCESS
  void stopPlayback() PREVIEW_STOP
  SYNC_METHOD
};
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
int main() {
  try {
    Scene scene;
    std::atomic_bool cancelled = false;
    audio::PagedPreviewSource::failConstruction = true;
    scene.process({.meta = {.BmsPath = "malformed.bms"}}, cancelled);
    require(!scene.selectedChart && !scene.previewDecodedAssets_ &&
                !scene.context.jukebox.loaded && !scene.previewPlayer_.state.playing,
            "preview construction failure must retire partial resources without escaping the worker");
    audio::PagedPreviewSource::failConstruction = false;
    scene.context.settings.audioVideo.audio.masterVolume = 0.5f;
    scene.context.settings.audioVideo.audio.bgmVolume = 0.4f;
    scene.context.settings.audioVideo.audio.keysoundVolume = 0.6f;
    scene.process({.meta = {.BmsPath = "chart.bms"}}, cancelled);
    require(audio::PagedPreviewSource::lastGains.bgm == 0.2f &&
                audio::PagedPreviewSource::lastGains.keysound == 0.3f,
            "master and bus gains must reach the paged source before overlap clipping");
    require(scene.previewDecodedAssets_ &&
                scene.previewDecodedAssets_->retainedLimit == audio::DecodedAssetCache::kUnlimitedRetainedBytes,
            "selected-chart preview must retain all prepared audio for gameplay");
    const auto &events = scene.context.jukebox.events;
    const auto audio = std::find(events.begin(), events.end(), "audio-start");
    const auto visual = std::find(events.begin(), events.end(), "visual-load");
    require(visual != events.end(), "progressive preview must prepare chart BGA");
    require(audio < visual, "progressive audio must start before BGA loading");
    require(scene.selectedChart && !scene.mediaReady,
            "visual preparation must not mark gameplay audio resources ready");
    const auto preparedCache = scene.previewDecodedAssets_;
    scene.stopPlayback();
    require(scene.previewDecodedAssets_ == preparedCache &&
                audio::PagedPreviewSource::lastCache == preparedCache,
            "stopping preview must retain its decoded cache for gameplay handoff");
    require(!scene.context.jukebox.loaded, "retired preview must release BGA");

    scene.context.jukebox.loaded = true; // The subsequent full gameplay load.
    scene.stopPlayback(); // onPause/cleanup can retire the worker again.
    require(scene.context.jukebox.loaded,
            "repeated preview stop must preserve the gameplay visual handoff");

    Scene handoffScene;
    handoffScene.context.jukebox.duringLoad = [&](std::atomic_bool &flag) {
      require(handoffScene.selectedChart != nullptr,
              "parsed chart/random choices must be published before slow BGA loading");
      handoffScene.willStart = true;
      flag = true;
    };
    handoffScene.process({.meta = {.BmsPath = "chart.bms"}}, cancelled);
    require(handoffScene.selectedChart && !handoffScene.mediaReady,
            "Play during visual loading must preserve the parsed chart for full preparation");
    require(!handoffScene.context.jukebox.loaded && !handoffScene.previewPlayer_.state.playing,
            "Play cancellation must retire only preview media");
    cancelled = false;

    Scene selectionScene;
    std::weak_ptr<bms_parser::Chart> loadingChart;
    selectionScene.context.jukebox.duringLoad = [&](std::atomic_bool &flag) {
      loadingChart = selectionScene.selectedChart;
      selectionScene.clearSelectedChart();
      flag = true;
      require(!loadingChart.expired(),
              "clearing a selection must not destroy the chart being loaded");
    };
    selectionScene.process({.meta = {.BmsPath = "chart.bms"}}, cancelled);
    require(loadingChart.expired() && !selectionScene.selectedChart,
            "cancelled load must release its final chart ownership");
    cancelled = false;

    Scene cancelledScene;
    cancelledScene.context.jukebox.duringLoad = [](std::atomic_bool &flag) { flag = true; };
    cancelledScene.process({.meta = {.BmsPath = "chart.bms"}}, cancelled);
    require(!cancelledScene.context.jukebox.loaded,
            "cancelled visual preparation must discard partially loaded BGA");
    require(!cancelledScene.previewPlayer_.state.playing,
            "cancelled visual preparation must stop its preview audio");
    require(!cancelledScene.selectedChart, "cancelled chart must not become reusable");
    Scene clockScene;
    clockScene.updatePreviewVisuals();
    require(clockScene.context.jukebox.positions.empty(), "idle preview must not seek gameplay BGA");
    clockScene.previewVisualsLoaded_ = true;
    clockScene.previewStartFrame_ = 2'205'000;
    clockScene.previewPlayer_.state = {.playing = true, .buffering = true,
                                       .positionFrames = 2'227'050};
    clockScene.updatePreviewVisuals();
    require(clockScene.context.jukebox.positions.back() == 50'500'000,
            "preview position is absolute: do not add previewStartFrame twice");
    clockScene.updatePreviewVisuals();
    require(clockScene.context.jukebox.positions.back() == 50'500'000,
            "buffering must freeze visuals at the last consumed frame");
    clockScene.previewPlayer_.state = {.finished = true, .positionFrames = 2'249'100};
    clockScene.updatePreviewVisuals();
    require(clockScene.context.jukebox.positions.back() == 51'000'000,
            "EOF must display the final consumed position");
    std::atomic_bool locked = false, release = false;
    std::thread loader([&] {
      std::lock_guard lock(clockScene.previewJukeboxLoadMutex);
      locked = true;
      while (!release) std::this_thread::yield();
    });
    while (!locked) std::this_thread::yield();
    clockScene.updatePreviewVisuals();
    release = true;
    loader.join();
    require(clockScene.context.jukebox.positions.size() == 3,
            "UI update must skip visual loading without blocking");
    clockScene.stopPlayback();
    clockScene.updatePreviewVisuals();
    require(clockScene.context.jukebox.positions.size() == 3,
            "retired preview must relinquish the visual clock");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
