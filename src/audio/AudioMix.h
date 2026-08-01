#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace player_settings {
struct AudioSettings;
}

namespace audio {

enum class Bus : std::uint8_t { Bgm, Keysound };

struct Volumes {
  float master = 1.0f;
  float bgm = 1.0f;
  float keysound = 1.0f;
};

float EffectiveGain(Bus bus, const Volumes &volumes);
Volumes VolumesFromSettings(const player_settings::AudioSettings &settings);

std::vector<short> ResamplePcm(std::span<const short> source, int channels,
                               int sourceRate, int targetRate);

} // namespace audio

struct SoundData {
  SoundData() = default;
  SoundData(const SoundData &) = delete;
  SoundData &operator=(const SoundData &) = delete;

  size_t currentFrame = 0;
  int channels = 0;
  int sourceSampleRate = 0;
  bool playing = false;
  std::vector<short> sourceData;
  std::vector<short> outputData;
  size_t sourceFrameCount = 0;
  size_t outputFrameCount = 0;
};

struct PlayingSound {
  SoundData *soundData = nullptr;
  audio::Bus bus = audio::Bus::Bgm;
  std::uint64_t sourceFrameQ32 = 0;
  std::uint32_t outputOffsetFrames = 0;
};

struct ScheduledSound {
  SoundData *soundData = nullptr;
  audio::Bus bus = audio::Bus::Bgm;
  long long startMicros = 0;
  std::uint64_t sequence = 0;
  size_t startFrame = 0;
};

enum class AudioCommandType : std::uint8_t { PlayNow, Schedule, StopAll };

struct AudioCommand {
  AudioCommandType type = AudioCommandType::StopAll;
  SoundData *soundData = nullptr;
  audio::Bus bus = audio::Bus::Bgm;
  long long startMicros = 0;
  std::uint64_t sequence = 0;
  size_t startFrame = 0;
};

constexpr size_t kMaxActiveSounds = 512;
constexpr size_t kMaxScheduledSounds = 65536;
constexpr size_t kAudioCommandQueueSize = 4096;
constexpr size_t kRealtimeAudioCommandQueueSize = 1024;

struct RealtimeAudioCommandReservation {
  std::uint32_t cursor = 0;
};

struct AudioCallbackState {
  AudioCallbackState();

  std::unique_ptr<PlayingSound[]> playingSounds;
  size_t playingSoundCount = 0;
  std::unique_ptr<ScheduledSound[]> scheduledSounds;
  size_t scheduledSoundCount = 0;
  std::unique_ptr<AudioCommand[]> commandQueue;
  std::atomic<std::uint32_t> commandReadCursor{0};
  std::atomic<std::uint32_t> commandWriteCursor{0};
  std::unique_ptr<AudioCommand[]> realtimeCommandQueue;
  std::atomic<std::uint32_t> realtimeCommandReadCursor{0};
  std::atomic<std::uint32_t> realtimeCommandWriteCursor{0};
};

namespace audio::playback {

struct OutputRateCandidate {
  SoundData *soundData = nullptr;
  std::vector<short> outputData;
  size_t outputFrameCount = 0;
};

struct OutputRateTransition {
  int previousSampleRate = 0;
  int targetSampleRate = 0;
  std::vector<OutputRateCandidate> candidates;
};

enum class BackendRunState : std::uint8_t { Stopped, Running, Unknown };

struct BackendStateObservation {
  BackendRunState state = BackendRunState::Unknown;
  std::string diagnostic;
};

struct BackendOperationResult {
  bool success = false;
  std::string diagnostic;
};

class IBackendLifecycle {
public:
  virtual ~IBackendLifecycle() = default;
  virtual BackendStateObservation observeState() const = 0;
  virtual int outputSampleRate() const = 0;
  virtual BackendOperationResult stopAndDrain() = 0;
  virtual BackendOperationResult start() = 0;
};

BackendStateObservation InterpretStoppedQueryResult(int result,
                                                    std::string diagnostic);
bool CanMutateCallbackStateDirectly(BackendRunState state) noexcept;
BackendOperationResult EnsureBackendStartedAtOutputRate(
    IBackendLifecycle &backend, std::span<SoundData *const> sounds,
    AudioCallbackState &callbackState, int targetSampleRate,
    std::atomic<int> &currentSampleRate,
    std::atomic<int64_t> &audioClockFrameCursor,
    std::atomic<BackendRunState> &backendState);
BackendOperationResult
StopBackendAndClearCallbackState(IBackendLifecycle &backend,
                                 AudioCallbackState &callbackState,
                                 std::atomic<BackendRunState> &backendState);

size_t RemapFramePosition(size_t frame, int previousSampleRate,
                          int targetSampleRate);
std::optional<OutputRateTransition>
PrepareOutputRateTransition(std::span<SoundData *const> sounds,
                            int previousSampleRate, int targetSampleRate);
void CommitOutputRateTransition(OutputRateTransition transition,
                                AudioCallbackState &state);

bool AppendActiveSound(AudioCallbackState &state, SoundData *soundData, Bus bus,
                       std::uint32_t outputOffsetFrames, size_t startFrame = 0);
bool InsertScheduledSound(AudioCallbackState &state,
                          const ScheduledSound &scheduledSound);
void ClearCallbackSounds(AudioCallbackState &state);
bool EnqueueCommand(AudioCallbackState &state, const AudioCommand &command);
std::optional<RealtimeAudioCommandReservation>
TryReserveRealtimeCommand(const AudioCallbackState &state) noexcept;
bool CommitRealtimeCommand(
    AudioCallbackState &state, RealtimeAudioCommandReservation reservation,
    const AudioCommand &command) noexcept;
void DrainRealtimeCommands(AudioCallbackState &state) noexcept;
void DrainCommands(AudioCallbackState &state);
void ActivateScheduledSounds(AudioCallbackState &state,
                             long long bufferStartMicros, int sampleRate,
                             std::uint32_t frameCount, int playbackRatePercent);
void MixActiveSounds(AudioCallbackState &state, std::span<float> mixBuffer,
                     std::uint32_t frameCount, int outputChannels,
                     float bgmGain, float keysoundGain,
                     int playbackRatePercent);

} // namespace audio::playback
