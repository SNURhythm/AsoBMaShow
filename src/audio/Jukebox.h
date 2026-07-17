#pragma once
#include "../bms_parser.hpp"
#include "../PrepMetronome.h"
#include "ClubBeat.h"
#include "AudioWrapper.h"
#include <algorithm>
#include <array>
#include <thread>
#include <unordered_map>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>
#include "../path.h"
#include "../video/VideoPlayer.h"
#include "../utils/Stopwatch.h"
#include <functional>

#include "../AppSettings.h"
#include <cassert>

struct ImageData {
  bgfx::TextureHandle texture;
  int width;
  int height;
  int channels;
};

using ChartResourceTable = std::unordered_map<int, std::string>;

struct ScheduledAudioEvent {
  long long timeMicros = 0;
  int wav = bms_parser::Parser::NoWav;
  audio::Bus bus = audio::Bus::Bgm;
};

enum class JukeboxAudioSource : std::uint8_t {
  BackgroundNote,
  ChartNote,
  DirectKeysound,
  ReplayKeysound,
  PrepMetronome,
  ClubBeat,
  SettingsTestTone,
};

constexpr audio::Bus
audioBusForJukeboxSource(JukeboxAudioSource source) noexcept {
  return source == JukeboxAudioSource::BackgroundNote ||
                 source == JukeboxAudioSource::ClubBeat
             ? audio::Bus::Bgm
             : audio::Bus::Keysound;
}

constexpr ScheduledAudioEvent
makeScheduledAudioEvent(long long timeMicros, int wav,
                        JukeboxAudioSource source) noexcept {
  return {.timeMicros = timeMicros,
          .wav = wav,
          .bus = audioBusForJukeboxSource(source)};
}

struct OverlappingAudioRequest {
  int wav = bms_parser::Parser::NoWav;
  long long offsetMicros = 0;
  audio::Bus bus = audio::Bus::Bgm;
};

inline std::optional<OverlappingAudioRequest>
makeOverlappingAudioRequest(const ScheduledAudioEvent &event,
                            long long seekMicros,
                            long long durationMicros) noexcept {
  if (seekMicros < event.timeMicros || durationMicros <= 0) {
    return std::nullopt;
  }
  const long long offsetMicros = seekMicros - event.timeMicros;
  if (offsetMicros >= durationMicros) {
    return std::nullopt;
  }
  return OverlappingAudioRequest{
      .wav = event.wav, .offsetMicros = offsetMicros, .bus = event.bus};
}

namespace audio::playback {
inline long long
SchedulerWaitMicrosForChartDelta(long long chartDeltaMicros, PlaybackRate rate,
                                 long long maximumWallSleepMicros) noexcept {
  if (chartDeltaMicros <= 0 || maximumWallSleepMicros <= 0) {
    return 0;
  }
  return std::min(maximumWallSleepMicros,
                  std::max(0LL, rate.realMicrosFromChart(chartDeltaMicros)));
}
} // namespace audio::playback

class Jukebox : public audio::IPlaybackSession {
public:
  struct PerformanceAnalytics {
    static const int BUFFER_SIZE = 10000;
    // ring buffer to store loop delta time
    std::array<std::atomic<uint32_t>, BUFFER_SIZE> loopDeltaTimes{};
    std::atomic<size_t> loopDeltaIndex{0};
    size_t cursor = 0;
    double avgDeltaTime = 0;
    double statsSum = 0;
    size_t statsCount = 0;
  };
  PerformanceAnalytics performanceAnalytics;

  Jukebox(Stopwatch *stopwatch);
  Jukebox(Stopwatch *stopwatch,
          std::unique_ptr<audio::IBackendFactory> backendFactory);
  ~Jukebox();

  // NOTE: Reading delta time is NOT THREAD SAFE, call this from render thread
  double getAvgDeltaTime();

  audio::playback::BackendOperationResult
  loadChart(bms_parser::Chart &chart, bool scheduleNotes,
            std::atomic_bool &isCancelled);
  audio::playback::BackendOperationResult
  reloadChartResources(bms_parser::Chart &chart, bool scheduleNotes,
                       std::atomic_bool &isCancelled);
  bool hasLoadedResources() const;
  void loadVisuals(bms_parser::Chart &chart, std::atomic_bool &isCancelled);
  void unloadVisuals();
  void schedule(bms_parser::Chart &chart, bool scheduleNotes,
                std::atomic_bool &isCancelled,
                std::optional<long long> noteScheduleCutoffMicros =
                    std::nullopt,
                const prep_metronome::PrepMetronomePlan *prepMetronomePlan =
                    nullptr,
                bool clubMode = false);
  void appendScheduledAudioEvents(
      std::span<const ScheduledAudioEvent> events);
  void seekVisualsToSongTime(long long rawSongMicros);
  void renderVisualsAt(long long micro);
  void playKeySound(int wav);
  [[nodiscard]] std::optional<audio::RealtimeSoundHandle>
  resolveRealtimeKeySound(int wav) const;
  audio::playback::BackendOperationResult play(long long startMicros = 0);
  audio::playback::BackendOperationResult stop();
  void render();
  bool hasActiveVisuals() const;
  long long getScheduledAudioEndMicros();
  long long getScheduledVisualEndMicros();
  std::vector<std::filesystem::path> activeMaterializedVideoPaths() const;
  void setVisualsEnabled(bool enabled);
  bool getVisualsEnabled() const;
  void setVisualsSuspended(bool suspended);
  bool getVisualsSuspended() const;
  void setBgaOffsetMs(int offsetMs);
  void setBgaDisplayMode(AppSettings::BgaDisplayMode mode);

  long long getTimeMicros();
  audio::playback::BackendOperationResult seek(long long micro);
  std::function<void(long long)> onTickCb;
  void onTick(const std::function<void(long long)> &cb) {
    assert(!isPlaying && "onTick callback should be set before playing");
    onTickCb = cb;
  }
  void removeOnTick() { onTickCb = nullptr; }
  void pause();
  void resume();
  bool isPaused();
  bool setPlaybackRate(audio::PlaybackRate rate, std::string &errorMessage);
  [[nodiscard]] audio::PlaybackRate playbackRate() const;
  [[nodiscard]] AudioWrapper &audioRuntime() { return audio; }
  audio::PlaybackSnapshot suspendAndDrain() override;
  bool restorePlayback(const audio::PlaybackSnapshot &snapshot,
                       std::string &errorMessage) override;
  void leavePlaybackStopped() override;

private:
  bgfx::UniformHandle s_texColor;
  // seek lock
  std::mutex seekLock;
  // playthread lock
  std::mutex playThreadLock;
  std::mutex schedulerWaitMutex;
  std::condition_variable schedulerWakeCv;
  void loadSounds(bms_parser::Chart &chart,
                  const ChartResourceTable &wavTable,
                  std::atomic_bool &isCancelled);
  bool loadArchivedSounds(bms_parser::Chart &chart,
                          const ChartResourceTable &wavTable,
                          std::atomic_bool &isCancelled);
  bool loadArchivedChartAssets(
      bms_parser::Chart &chart, const ChartResourceTable &wavTable,
      const ChartResourceTable &bmpTable, bool loadVisualAssets,
      std::atomic_bool &isCancelled,
      audio::playback::BackendOperationResult &lifecycleResult);
  void loadBMPs(bms_parser::Chart &chart,
                const ChartResourceTable &bmpTable,
                std::atomic_bool &isCancelled);
  bool loadArchivedBMPs(bms_parser::Chart &chart,
                        const ChartResourceTable &bmpTable,
                        std::atomic_bool &isCancelled);
  bool loadVideoPath(int id, const std::filesystem::path &path,
                     std::atomic_bool &isCancelled);
  bool loadMaterializedVideoPath(int id,
                                 const std::filesystem::path &materializedPath,
                                 const std::filesystem::path &displayPath,
                                 std::atomic_bool &isCancelled);
  bool loadImagePath(int id, const std::filesystem::path &path,
                     std::atomic_bool &isCancelled);
  bool loadImageBytes(int id, const std::filesystem::path &path,
                      const std::vector<unsigned char> &bytes,
                      std::atomic_bool &isCancelled);
  void clearVisualResources();
  void scheduleVisuals(bms_parser::Chart &chart,
                       std::atomic_bool &isCancelled);
  struct ResolvedSoundAsset {
    int id = 0;
    std::filesystem::path path;
    path_t key;
  };
  struct ResolvedVisualAsset {
    int id = 0;
    std::filesystem::path path;
    path_t key;
    bool video = false;
  };
  std::vector<ResolvedSoundAsset>
  resolveSoundAssets(bms_parser::Chart &chart,
                     const ChartResourceTable &wavTable,
                     std::atomic_bool &isCancelled);
  std::vector<ResolvedVisualAsset>
  resolveVisualAssets(bms_parser::Chart &chart,
                      const ChartResourceTable &bmpTable,
                      std::atomic_bool &isCancelled);
  audio::playback::BackendOperationResult loadResolvedChartResources(
      bms_parser::Chart &chart, const ChartResourceTable &wavTable,
      const ChartResourceTable &bmpTable, bool loadVisualAssets,
      std::atomic_bool &isCancelled);
  audio::playback::BackendOperationResult
  reconcileSoundResources(bms_parser::Chart &chart,
                          const std::vector<ResolvedSoundAsset> &assets,
                          std::atomic_bool &isCancelled);
  void reconcileVisualResources(
      bms_parser::Chart &chart, const std::vector<ResolvedVisualAsset> &assets,
      std::atomic_bool &isCancelled);
  bool scheduleAudioFromCursor(size_t cursor);
  void playOverlappingAudioAt(long long micro);
  audio::playback::BackendOperationResult
  playWithClockState(long long startMicros, bool paused);
  void ensurePrepMetronomeSoundsLoaded();
  void ensureClubBeatSoundsLoaded();
  void wakeScheduler();
  void syncVisualClockToAudio();
  [[nodiscard]] long long getBgaOffsetMicros() const;
  [[nodiscard]] long long getBgaTimelineMicros(long long rawSongMicros) const;
  [[nodiscard]] long long
  getRawSongMicrosForBgaTarget(long long bgaTargetMicros) const;
  bool activateVisual(int visualId, bgfx::ViewId viewId);
  bool activateVisualAt(int visualId, bgfx::ViewId viewId,
                        long long elapsedMicros);
  void restoreVisualsAtTimelineMicrosLocked(long long bgaTimelineMicros);
  void advanceVisualsAtTimelineMicros(long long bgaTimelineMicros);
  void renderImage(ImageData &image, int viewId);
  struct BgaRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
  };
  BgaRect calculateBgaRect(int sourceWidth, int sourceHeight) const;
  std::atomic_bool isPlaying = false;
  std::atomic_bool schedulerActive = false;
  std::thread playThread;
  Stopwatch *stopwatch;
  AudioWrapper audio;
  std::vector<ScheduledAudioEvent> audioList;
  size_t audioCursor = 0;
  std::vector<std::pair<long long, int>> bmpList;
  std::vector<std::pair<long long, int>> bmpLayerList;
  size_t bmpCursor = 0;
  size_t bmpLayerCursor = 0;
  long long lastVisualTimelineMicros = -1;
  std::unordered_map<int, path_t> wavTableAbs;
  std::unordered_map<int, std::unique_ptr<VideoPlayer>> videoPlayerTable;
  std::unordered_map<int, std::filesystem::path> videoMaterializedPathTable;
  mutable std::mutex videoPlayerTableMutex;
  std::unordered_map<int, ImageData> imageTable;
  mutable std::mutex imageTableMutex;
  std::unordered_map<int, path_t> visualPathTable;
  std::atomic<int> currentBga{-1};
  std::atomic<int> currentBmpLayer{-1};
  std::atomic_bool visualsEnabled{true};
  std::atomic_bool visualsSuspended{false};
  std::atomic<int> bgaOffsetMs{0};
  std::atomic<int> bgaDisplayMode{
      static_cast<int>(AppSettings::BgaDisplayMode::Fit)};
  const std::string videoExtensions[9] = {"mp4",  "wmv", "m4v", "webm", "mpg",
                                          "mpeg", "m1v", "m2v", "avi"};
  const std::string imageExtensions[6] = {"jpg", "jpeg", "gif",
                                          "bmp", "png",  "tga"};
};
