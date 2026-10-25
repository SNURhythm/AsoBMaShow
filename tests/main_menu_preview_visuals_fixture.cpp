#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>
#include "ui/PresentationSchedule.h"
#include "RAII.h"
#include "audio/ChartPreviewPolicy.h"
#include "audio/AudioSpectrum.h"
#include "music_select/MusicSelectPreview.h"

// Run the production worker and clock adapter with device/parser boundaries
// replaced, so no native audio device or renderer is needed for this wiring test.
struct ChartMetaRecord { struct { std::filesystem::path BmsPath, Preview; } meta; };
namespace bms_parser { struct Chart { struct { std::filesystem::path BmsPath, Preview; } Meta; }; }
namespace audio {
struct DecodedAssetCache {
  static constexpr std::size_t kUnlimitedRetainedBytes = std::numeric_limits<std::size_t>::max();
  explicit DecodedAssetCache(std::size_t limit) : retainedLimit(limit) {}
  std::size_t retainedLimit;
  bool cleared = false;
  void Clear() { cleared = true; }
};
}
namespace audio {
inline long long FindChartPreviewHighlightMicros(const bms_parser::Chart &) { return 40'000'000; }
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
  struct State {
    bool preparationComplete = true, cancelled = false;
    std::uint64_t revision = 0;
    std::string error;
  };
  static inline bool preparationComplete = true;
  static inline std::function<void()> duringWait;
  State GetState() const { return {.preparationComplete = preparationComplete}; }
  void WaitForChange(std::uint64_t, std::chrono::milliseconds) { if (duringWait) duringWait(); }
  void Cancel() {}
};
}
namespace play_options {
inline std::filesystem::path previewPath;
std::unique_ptr<bms_parser::Chart> parseChart(std::filesystem::path path,
    std::atomic_bool &, const char *) {
  auto chart = std::make_unique<bms_parser::Chart>();
  chart->Meta.BmsPath = path;
  chart->Meta.Preview = previewPath;
  return chart;
}
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
  bool fadeIn = false;
  bool Start(std::shared_ptr<audio::PagedPreviewSource> source, std::string &, bool fade) {
    fadeIn = fade;
    events->push_back("audio-start");
    state = {.playing = !fail, .positionFrames = source->first};
    return !fail;
  }
  void Stop() { state = {}; events->push_back("audio-stop"); }
  void SetRequest(Request) {}
  void SetGain(float) {}
  State GetState() const { return state; }
  float GetOutputLevel() const { return state.playing ? 0.4F : 0; }
  audio::Spectrum GetOutputSpectrum() const { return {0.4F}; }
};
struct FilePlayer {
  explicit FilePlayer(void *, std::filesystem::path, bool analyze = false) : analyze(analyze) {}
  bool analyze;
  bool available = false, playing = false;
  float gain = 1;
  std::filesystem::path playedPath;
  bool play(const std::filesystem::path &path, bool loop, std::atomic_bool &,
            std::stop_token = {}, float volume = 1) {
    if (!loop) throw std::runtime_error("preview files must loop");
    playedPath = path;
    gain = volume;
    return playing = available;
  }
  void stopCurrent() { playing = false; }
  float outputLevel() const { return playing && analyze ? gain * 0.6F : 0; }
  audio::Spectrum outputSpectrum() const { return {playing && analyze ? gain * 0.8F : 0}; }
};
using MusicSelectPreviewFilePlayer = FilePlayer;
struct Visuals {
  Visuals &audioRuntime() { return *this; }
  std::vector<std::string> events;
  bool loaded = true;
  std::function<void(std::atomic_bool &)> duringLoad;
  std::vector<long long> positions;
  std::shared_ptr<audio::DecodedAssetCache> cache;
  bool failPreparation = false;
  std::function<void(std::atomic_bool &)> duringPreparation;
  void setDecodedAssetCache(std::shared_ptr<audio::DecodedAssetCache> value) { cache = std::move(value); }
  struct Result { bool success; std::string diagnostic; };
  int ratePercent = 100;
  bool setPlaybackRate(int, std::string &) { ratePercent = 100; return true; }
  Result loadChartPreservingDevice(bms_parser::Chart &, bool, std::atomic_bool &) {
    if (ratePercent != 100) throw std::runtime_error("chart load stops a file started at a non-neutral rate");
    loaded = true;
    return {true};
  }
  Result prepareGameplayAudioFromPreview(bms_parser::Chart &, std::atomic_bool &cancelled) {
    if (!cache || !loaded) throw std::runtime_error("preparation must reuse preview audio and visuals");
    events.push_back("gameplay-prepare");
    if (duringPreparation) duringPreparation(cancelled);
    return {!failPreparation, "injected failure"};
  }
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
struct ReadyFlag {
  std::atomic_bool value = false;
  std::function<void()> onReady;
  bool load() const { return value.load(); }
  operator bool() const { return load(); }
  void store(bool ready) { value.store(ready); if (ready && onReady) onReady(); }
  ReadyFlag &operator=(bool ready) { store(ready); return *this; }
};
struct Details {
  float level = 0;
  audio::Spectrum spectrum{};
  void updatePreviewGlow(float value, float, std::function<audio::Spectrum()> read) {
    level = value;
    spectrum = value > 0 ? read() : audio::Spectrum{};
  }
};
struct Scene {
  struct Context {
    Visuals jukebox;
    struct Settings {
      audio::ChartPreviewMode chartPreviewMode = audio::ChartPreviewMode::Highlight;
      bool chartPreviewFallbackHighlight = true;
      struct AudioVideo {
        struct Audio {
          float masterVolume = 1, bgmVolume = 1, keysoundVolume = 1;
          std::string outputDeviceId;
          unsigned requestedSampleRate = 48000, requestedBufferFrames = 128;
        } audio;
      } audioVideo;
    } settings;
  } context;
  enum class Tab { Play, Settings };
  Tab activeTab_ = Tab::Play;
  std::optional<ChartMetaRecord> selectedChartRecord;
  std::unique_ptr<Details> chartDetailsView_ = std::make_unique<Details>();
  Player previewPlayer_;
  std::unique_ptr<FilePlayer> previewFilePlayer_;
  std::shared_ptr<audio::DecodedAssetCache> previewDecodedAssets_;
  std::uint64_t previewStartFrame_ = 0;
  bool previewVisualsLoaded_ = false;
  std::optional<std::uint64_t> previewVisualPositionFrames_;
  std::mutex previewJukeboxLoadMutex;
  std::mutex previewVisualsMutex;
  std::unique_ptr<Worker> previewWorker_ = std::make_unique<Worker>();
  std::atomic_bool willStart = false;
  std::mutex selectedChartMutex;
  std::shared_ptr<bms_parser::Chart> selectedChart;
  ReadyFlag selectedChartMediaReady;
  Scene() { previewPlayer_.events = &context.jukebox.events; FILE_PLAYER_CONSTRUCTION }
  void setSelectedChart(std::shared_ptr<bms_parser::Chart> chart, bool ready) {
    selectedChart = std::move(chart); selectedChartMediaReady = ready;
  }
  void clearSelectedChart() { std::lock_guard lock(selectedChartMutex); selectedChart.reset(); selectedChartMediaReady = false; }
  void process(const ChartMetaRecord &request, std::atomic_bool &cancelled)
    PREVIEW_PROCESS
  void stopPlayback() PREVIEW_STOP
  SYNC_METHOD
  GLOW_METHOD
};
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
int main() {
  try {
    Scene scene;
    std::atomic_bool cancelled = false;
    {
      Scene modes;
      modes.process({.meta = {.BmsPath = "chart.bms"}}, cancelled);
      require(modes.previewPlayer_.state.positionFrames == 40 * 44100 && modes.previewPlayer_.fadeIn,
              "default preview selects the highlight with fade-in");
      modes.context.settings.chartPreviewMode = audio::ChartPreviewMode::Beginning;
      modes.process({.meta = {.BmsPath = "chart.bms"}}, cancelled);
      require(modes.previewPlayer_.state.positionFrames == 0 && !modes.previewPlayer_.fadeIn,
              "Beginning preserves original playback");
      modes.context.settings.chartPreviewMode = audio::ChartPreviewMode::PreviewFile;
      modes.context.settings.chartPreviewFallbackHighlight = false;
      play_options::previewPath = "preview.ogg";
      modes.process({.meta = {.BmsPath = "chart.bms", .Preview = "preview.ogg"}}, cancelled);
      require(modes.previewPlayer_.state.playing && modes.previewPlayer_.state.positionFrames == 0,
              "failed authored file uses the chosen Beginning fallback");
      modes.previewFilePlayer_->available = true;
      modes.context.jukebox.ratePercent = 150;
      modes.process({.meta = {.BmsPath = "chart.bms", .Preview = "preview.ogg"}}, cancelled);
      require(modes.previewFilePlayer_->playing && !modes.previewPlayer_.state.playing && modes.selectedChartMediaReady,
              "usable preview file loops and still prepares gameplay resources");
      modes.selectedChartRecord = ChartMetaRecord{.meta = {.BmsPath = "chart.bms"}};
      modes.updateGlow(0.016F);
      require(modes.chartDetailsView_->level == 0.6F && modes.chartDetailsView_->spectrum[0] == 0.8F,
              "authored preview output drives both jacket pulse and spectrum");
      modes.selectedChartRecord->meta.BmsPath = "next.bms";
      modes.updateGlow(0.016F);
      require(modes.chartDetailsView_->level == 0, "old file audio cannot pulse the next selection");
      modes.selectedChartRecord->meta.BmsPath = "chart.bms";
      for (const float master : {0.0F, 0.25F}) {
        modes.context.settings.audioVideo.audio.masterVolume = master;
        modes.process({.meta = {.BmsPath = "chart.bms", .Preview = "preview.ogg"}}, cancelled);
        require(modes.previewFilePlayer_->playing && modes.previewFilePlayer_->gain == master,
                "authored preview must obey master volume, including mute");
      }
      modes.stopPlayback();
      require(!modes.previewFilePlayer_->playing, "handoff stops the authored preview");
      play_options::previewPath.clear();
      modes.process({.meta = {.BmsPath = "song/chart.bms", .Preview = "preview.ogg"}}, cancelled);
      require(modes.previewFilePlayer_->playing && !modes.previewPlayer_.state.playing &&
                  modes.previewFilePlayer_->playedPath == "song/preview.ogg",
              "Preview File uses the library's discovered file when the chart has no PREVIEW header");
    }
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
    require(audio::PagedPreviewSource::lastCache->retainedLimit ==
                audio::DecodedAssetCache::kUnlimitedRetainedBytes &&
                audio::PagedPreviewSource::lastCache->cleared && !scene.previewDecodedAssets_,
            "completed gameplay handoff releases preparation cache ownership");
    const auto &events = scene.context.jukebox.events;
    const auto audio = std::find(events.begin(), events.end(), "audio-start");
    const auto visual = std::find(events.begin(), events.end(), "visual-load");
    require(visual != events.end(), "progressive preview must prepare chart BGA");
    require(audio < visual, "progressive audio must start before BGA loading");
    require(scene.selectedChart && scene.selectedChartMediaReady,
            "completed preview must prepare gameplay resources before Play");
    scene.willStart = true;
    require(!scene.context.jukebox.cache, "preparation must release its cache attachment");
    scene.stopPlayback();
    require(!scene.previewDecodedAssets_ && audio::PagedPreviewSource::lastCache->cleared,
            "stopping a fully prepared preview does not retain a redundant decode cache");
    require(scene.context.jukebox.loaded, "Play must retain prepared preview BGA");

    scene.context.jukebox.loaded = true; // The subsequent full gameplay load.
    scene.stopPlayback(); // onPause/cleanup can retire the worker again.
    require(scene.context.jukebox.loaded,
            "repeated preview stop must preserve the gameplay visual handoff");

    Scene preparingScene;
    preparingScene.context.jukebox.duringPreparation = [&](std::atomic_bool &) {
      // Audio decoding already finished before visual loading. Gameplay staging
      // is still blocked here while the UI activates and advances the preview.
      std::thread uiThread([&] {
        preparingScene.previewPlayer_.state.positionFrames = 4410;
        preparingScene.updatePreviewVisuals();
        preparingScene.previewPlayer_.state.positionFrames = 8820;
        preparingScene.updatePreviewVisuals();
      });
      uiThread.join();
    };
    preparingScene.process({.meta = {.BmsPath = "chart.bms"}}, cancelled);
    require(preparingScene.context.jukebox.positions == std::vector<long long>{100'000, 200'000},
            "preview must activate and advance while gameplay audio preparation is blocked");

    Scene progressiveScene;
    audio::PagedPreviewSource::preparationComplete = false;
    audio::PagedPreviewSource::duringWait = [&] {
      require(progressiveScene.previewPlayer_.state.playing && !progressiveScene.selectedChartMediaReady,
              "preview must play before gameplay preparation finishes");
      require(progressiveScene.previewJukeboxLoadMutex.try_lock(),
              "waiting for audio must leave preview visuals responsive");
      progressiveScene.previewJukeboxLoadMutex.unlock();
      require(std::find(progressiveScene.context.jukebox.events.begin(),
                        progressiveScene.context.jukebox.events.end(), "gameplay-prepare") ==
                  progressiveScene.context.jukebox.events.end(),
              "gameplay staging must not compete with progressive audio decoding");
      audio::PagedPreviewSource::preparationComplete = true;
    };
    progressiveScene.process({.meta = {.BmsPath = "chart.bms"}}, cancelled);
    require(progressiveScene.selectedChartMediaReady, "completed progressive decode leads to gameplay readiness");
    audio::PagedPreviewSource::duringWait = {};

    Scene cancelledWait;
    audio::PagedPreviewSource::preparationComplete = false;
    audio::PagedPreviewSource::duringWait = [&] { cancelled = true; };
    cancelledWait.process({.meta = {.BmsPath = "chart.bms"}}, cancelled);
    require(!cancelledWait.selectedChart && !cancelledWait.previewPlayer_.state.playing,
            "selection cancellation must interrupt the preparation wait");
    audio::PagedPreviewSource::preparationComplete = true;
    audio::PagedPreviewSource::duringWait = {};
    cancelled = false;

    Scene stoppedScene;
    stoppedScene.process({.meta = {.BmsPath = "chart.bms"}}, cancelled);
    stoppedScene.stopPlayback();
    require(!stoppedScene.selectedChartMediaReady && !stoppedScene.context.jukebox.loaded,
            "stopping without Play must retire visuals and invalidate readiness");

    Scene failedScene;
    failedScene.context.jukebox.failPreparation = true;
    failedScene.process({.meta = {.BmsPath = "chart.bms"}}, cancelled);
    require(!failedScene.selectedChartMediaReady && !failedScene.context.jukebox.cache,
            "failed gameplay preparation must remain retryable and release its cache attachment");

    Scene cancelledPreparation;
    cancelledPreparation.context.jukebox.duringPreparation = [&](std::atomic_bool &flag) {
      cancelledPreparation.willStart = true;
      flag = true;
    };
    cancelledPreparation.process({.meta = {.BmsPath = "chart.bms"}}, cancelled);
    require(cancelledPreparation.selectedChart && !cancelledPreparation.selectedChartMediaReady &&
                !cancelledPreparation.context.jukebox.loaded && !cancelledPreparation.context.jukebox.cache,
            "Play during audio preparation keeps the chart but never publishes partial media as ready");
    require(cancelledPreparation.previewDecodedAssets_ &&
                !cancelledPreparation.previewDecodedAssets_->cleared,
            "Play cancelling gameplay preparation must retain decoded PCM for foreground completion");
    cancelled = false;

    Scene publicationRace;
    publicationRace.selectedChartMediaReady.onReady = [&] {
      publicationRace.willStart = true;
      cancelled = true;
    };
    publicationRace.process({.meta = {.BmsPath = "chart.bms"}}, cancelled);
    publicationRace.stopPlayback();
    require(!publicationRace.selectedChartMediaReady || publicationRace.context.jukebox.loaded,
            "Play racing readiness publication must never reuse unloaded visuals");
    cancelled = false;

    Scene handoffScene;
    handoffScene.context.jukebox.duringLoad = [&](std::atomic_bool &flag) {
      require(handoffScene.selectedChart != nullptr,
              "parsed chart/random choices must be published before slow BGA loading");
      handoffScene.willStart = true;
      flag = true;
    };
    handoffScene.process({.meta = {.BmsPath = "chart.bms"}}, cancelled);
    require(handoffScene.selectedChart && !handoffScene.selectedChartMediaReady,
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
    ui::PresentationSchedule schedule;
    schedule.didPresent(ui::revisions(), {}, 0);
    clockScene.updatePreviewVisuals();
    require(clockScene.context.jukebox.positions.back() == 50'500'000,
            "buffering must freeze visuals at the last consumed frame");
    require(!schedule.shouldRender(1), "unchanged preview position must settle");
    clockScene.previewPlayer_.state = {.finished = true, .positionFrames = 2'249'100};
    clockScene.updatePreviewVisuals();
    require(schedule.shouldRender(1), "preview EOF must wake drawing for its final BGA position");
    require(clockScene.context.jukebox.positions.back() == 51'000'000,
            "EOF must display the final consumed position");
    std::atomic_bool locked = false, release = false;
    std::thread loader([&] {
      std::lock_guard lock(clockScene.previewVisualsMutex);
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
