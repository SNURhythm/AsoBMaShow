#pragma once
#include "../bms_parser.hpp"
#include "../PrepMetronome.h"
#include "ClubBeat.h"
#include "AudioWrapper.h"
#include "GameplayBgaFrame.h"
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

namespace rendering {
class BgfxVertexLayoutRegistration;
}

struct ImageData {
  bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
  int width = 0;
  int height = 0;
  int channels = 0;
  bgfx::TextureHandle layerTexture = BGFX_INVALID_HANDLE;
};

// Creates the pinned TYPE_LAYER representation without relying on a runtime
// layer shader: exact black becomes transparent and every other RGBA pixel is
// retained verbatim.
[[nodiscard]] std::vector<std::uint8_t>
MakeGameplayBgaLayerRgba(std::span<const std::uint8_t> rgba);

// Transfers a loader-guarded texture into the shared frame/image table owner.
// On failure before transfer, image remains owning; after transfer, image is
// invalid and the raw/shared deleter is the sole owner.
[[nodiscard]] std::shared_ptr<ImageData>
AdoptImageTextureToSharedOwner(ImageData &image);

using ChartResourceTable = std::unordered_map<int, std::string>;

// One channel-06 sequence as authored at a measure's position-zero timeline.
// The frame vector is deliberately verbatim: BgaSequenceBlank, repeated IDs,
// and even empty sequences are significant to the compatibility layer.
struct ScheduledBgaPoorSequence {
  long long startMicros = 0;
  std::vector<int> frames;

  bool operator==(const ScheduledBgaPoorSequence &) const = default;
};

using BgaPoorSequenceSchedule = std::vector<ScheduledBgaPoorSequence>;

struct GameplayBgaResolvedQuad {
  std::array<GameplayBgaPoint, 4> destination{};
  std::array<GameplayBgaPoint, 4> uvs{};
};

[[nodiscard]] std::optional<GameplayBgaResolvedQuad>
ResolveGameplayBgaTargetQuad(const BgaDrawTarget &target, int sourceWidth,
                             int sourceHeight) noexcept;

[[nodiscard]] BgaPoorSequenceSchedule
BuildBgaPoorSequenceSchedule(const bms_parser::Chart &chart);

// Returns the latest sequence whose start is at or before timelineMicros.
// Callers recompute this from the immutable schedule after every seek.
[[nodiscard]] std::optional<std::size_t>
SelectBgaPoorSequenceIndexAt(const BgaPoorSequenceSchedule &schedule,
                             long long timelineMicros) noexcept;

// A sequence boundary is strictly after timelineMicros. This is kept separate
// from miss-trigger timing because chart selection must not mutate on a judge.
[[nodiscard]] std::optional<long long>
NextBgaPoorSequenceStartAfter(const BgaPoorSequenceSchedule &schedule,
                              long long timelineMicros) noexcept;

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

class Jukebox : public audio::IPlaybackSession,
                public IGameplayBgaSubmitter {
public:
  struct GameplayBgaSubmissionStats {
    std::uint64_t preparedFrames = 0;
    std::uint64_t videoUpdates = 0;
    std::uint64_t embeddedSubmissions = 0;
    std::uint64_t fullscreenSubmissions = 0;
    std::uint64_t pinnedFrames = 0;
  };

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
  // Like loadChart but keeps the shared audio device running when the jukebox
  // owns no active playback and the playback rate is neutral, so Bus::System
  // voices (the music-select BGM / preview / SE) are not torn down while the
  // chart is staged. Falls back to a normal full-stop load otherwise. Used by
  // the music-select preload.
  audio::playback::BackendOperationResult
  loadChartPreservingDevice(bms_parser::Chart &chart, bool scheduleNotes,
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
  // Stops the playback session but keeps the audio device open, so a later
  // play() for the same loaded chart short-circuits the device start.
  audio::playback::BackendOperationResult stopKeepDevice();
  void render();
  bool hasActiveVisuals() const;
  long long getScheduledAudioEndMicros();
  long long getScheduledVisualEndMicros();
  std::vector<std::filesystem::path> activeMaterializedVideoPaths() const;
  void setVisualsEnabled(bool enabled);
  bool getVisualsEnabled() const;
  void setVisualsSuspended(bool suspended);
  bool getVisualsSuspended() const;
  void handleMemoryPressure();
  void setBgaOffsetMs(int offsetMs);
  void setBgaDisplayMode(AppSettings::BgaDisplayMode mode);
  void setEmbeddedBgaBrightnessPercent(int percent) noexcept;
  [[nodiscard]] float embeddedBgaBrightnessMultiplier() const noexcept;
  [[nodiscard]] std::array<float, 4>
  embeddedBgaTint(const std::array<float, 4> &authoredTint) const noexcept;
  [[nodiscard]] std::uint64_t
  gameplayBgaProgramLookupCount() const noexcept {
    return bgaProgramLookupCount;
  }
  [[nodiscard]] PreparedGameplayBgaFrame prepareVisualFrameAt(
      std::uint64_t frameSerial, std::int64_t bgaTimeMicros,
      const GameplayBgaMissState &missState) override;
  [[nodiscard]] BgaPreflightResult
  preflight(const PreparedGameplayBgaFrame &frame,
            std::span<const BgaDrawTarget> targets) override;
  void commitPrepared(
      const PreparedGameplayBgaFrame &frame) noexcept override;
  void submitPrepared(const PreparedGameplayBgaFrame &frame,
                      const BgaDrawTarget &target) noexcept override;
  void finalizePrepared(
      const PreparedGameplayBgaFrame &frame) noexcept override;
  void submitFullscreen(const PreparedGameplayBgaFrame &frame) noexcept override;
  [[nodiscard]] GameplayBgaSubmissionStats
  gameplayBgaSubmissionStats() const noexcept;
  [[nodiscard]] const BgaPoorSequenceSchedule &
  poorBgaSequenceSchedule() const noexcept {
    return poorBgaSequences;
  }
  [[nodiscard]] std::optional<std::size_t>
  poorBgaSequenceIndexAt(long long timelineMicros) const noexcept {
    return SelectBgaPoorSequenceIndexAt(poorBgaSequences, timelineMicros);
  }

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
  [[nodiscard]] bgfx::ProgramHandle
  prepareGameplayBgaProgram(const char *vertexShader,
                            const char *fragmentShader) noexcept;
  bgfx::UniformHandle s_texColor;
  bgfx::ProgramHandle bgaPlaceholderProgram = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle bgaEmbeddedImageProgram = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle bgaFullscreenImageProgram = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle bgaFullscreenLayerProgram = BGFX_INVALID_HANDLE;
  std::uint64_t bgaProgramLookupCount = 0;
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
  bool prepareGameplayBgaLayerImageIds(
      const bms_parser::Chart &chart,
      const std::vector<ResolvedVisualAsset> &assets,
      std::atomic_bool &isCancelled) noexcept;
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
  bool preloadVisual(int visualId, std::atomic_bool &isCancelled);
  audio::playback::BackendOperationResult
  loadChartImpl(bms_parser::Chart &chart, bool scheduleNotes,
                std::atomic_bool &isCancelled, bool preserveDevice);
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
  struct PinnedGameplayBgaSurface {
    PreparedGameplayBgaSurface descriptor;
    std::shared_ptr<VideoPlayer> video;
    std::shared_ptr<ImageData> image;
  };
  struct PreparedBgaFrameLease {
    std::uint64_t frameSequence = 0;
    std::optional<PinnedGameplayBgaSurface> base;
    std::optional<PinnedGameplayBgaSurface> layer;
    std::optional<PinnedGameplayBgaSurface> miss;
  };
  [[nodiscard]] std::optional<PinnedGameplayBgaSurface>
  prepareGameplayBgaSurface(GameplayBgaRole role, int visualId,
                            std::unordered_set<int> &updatedVideoIds);
  void submitFullscreenSurface(const PinnedGameplayBgaSurface &surface,
                               bgfx::ViewId viewId) noexcept;
  void invalidatePreparedBgaPlans() noexcept;
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
  std::unordered_set<int> gameplayBgaLayerImageIds;
  BgaPoorSequenceSchedule poorBgaSequences;
  size_t bmpCursor = 0;
  size_t bmpLayerCursor = 0;
  long long lastVisualTimelineMicros = -1;
  std::unordered_map<int, path_t> wavTableAbs;
  std::unordered_map<int, std::shared_ptr<VideoPlayer>> videoPlayerTable;
  std::unordered_map<int, std::filesystem::path> videoMaterializedPathTable;
  mutable std::mutex videoPlayerTableMutex;
  std::unordered_map<int, std::shared_ptr<ImageData>> imageTable;
  mutable std::mutex imageTableMutex;
  std::unordered_map<int, path_t> visualPathTable;
  std::unordered_map<int, ResolvedVisualAsset> visualDescriptors;
  mutable std::mutex visualMaterializationMutex;
  std::atomic<int> currentBga{-1};
  std::atomic<int> currentBmpLayer{-1};
  std::atomic_bool visualsEnabled{true};
  std::atomic_bool visualsSuspended{false};
  std::atomic<int> bgaOffsetMs{0};
  std::atomic<int> bgaDisplayMode{
      static_cast<int>(AppSettings::BgaDisplayMode::Fit)};
  std::atomic<float> embeddedBgaBrightness{1.0F};
  const std::string videoExtensions[9] = {"mp4",  "wmv", "m4v", "webm", "mpg",
                                          "mpeg", "m1v", "m2v", "avi"};
  const std::string imageExtensions[6] = {"jpg", "jpeg", "gif",
                                          "bmp", "png",  "tga"};
  struct PreparedBgaFrameKey {
    std::uint64_t frameSerial = 0;
    std::int64_t bgaTimeMicros = 0;
    GameplayBgaMissState missState;

    [[nodiscard]] bool
    operator==(const PreparedBgaFrameKey &other) const noexcept {
      return frameSerial == other.frameSerial &&
             bgaTimeMicros == other.bgaTimeMicros &&
             missState.active == other.missState.active &&
             missState.startedBgaMicros == other.missState.startedBgaMicros &&
             missState.durationMicros == other.missState.durationMicros &&
             missState.triggerSerial == other.missState.triggerSerial;
    }
  };
  struct PreparedBgaTargetPlan {
    std::uint64_t frameSequence = 0;
    BgaDrawTarget target;
    std::optional<PreparedGameplayBgaSurface> surface;
    std::shared_ptr<VideoPlayer> video;
    std::shared_ptr<ImageData> image;
    std::optional<VideoPlayer::PreparedEmbeddedSubmission> videoSubmission;
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle imageTexture = BGFX_INVALID_HANDLE;
    std::uint32_t imageSamplerFlags = BGFX_SAMPLER_UVW_CLAMP;
    GameplayBgaMaterial material;
    std::array<float, 4> embeddedTint{1.0F, 1.0F, 1.0F, 1.0F};
    bgfx::TransientVertexBuffer vertexBuffer{};
    bgfx::TransientIndexBuffer indexBuffer{};
    bool reserved = false;
    std::array<GameplayBgaPoint, 4> destination{};
    std::array<GameplayBgaPoint, 4> uvs{};
    std::optional<rendering::DrawableScissor> scissor;
    bool placeholder = false;
    bool noDraw = false;
  };
  std::mutex preparedBgaFrameMutex;
  std::optional<PreparedBgaFrameKey> preparedBgaFrameKey;
  std::optional<PreparedGameplayBgaFrame> preparedBgaFrame;
  std::unordered_map<std::uint64_t, PreparedBgaFrameLease>
      preparedBgaFrameLeases;
  std::uint64_t preparedBgaSequence = 0;
  std::vector<PreparedBgaTargetPlan> preparedBgaTargetPlans;
  std::unique_ptr<rendering::BgfxVertexLayoutRegistration>
      preparedBgaVertexLayouts;
  std::optional<std::uint64_t> preparedBgaTargetFrameSequence;
  std::size_t preparedBgaTargetCursor = 0;
  bool preparedBgaTargetsCommitted = false;
  std::atomic<std::uint64_t> preparedGameplayBgaFrames{0};
  std::atomic<std::uint64_t> preparedGameplayBgaVideoUpdates{0};
  std::atomic<std::uint64_t> embeddedGameplayBgaSubmissions{0};
  std::atomic<std::uint64_t> fullscreenGameplayBgaSubmissions{0};
  std::atomic<std::uint64_t> pinnedGameplayBgaFrames{0};
};
