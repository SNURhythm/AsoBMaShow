#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
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
  size_t currentFrame = 0;
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

struct AudioCallbackState {
  AudioCallbackState();

  std::unique_ptr<PlayingSound[]> playingSounds;
  size_t playingSoundCount = 0;
  std::unique_ptr<ScheduledSound[]> scheduledSounds;
  size_t scheduledSoundCount = 0;
  std::unique_ptr<AudioCommand[]> commandQueue;
  std::atomic<std::uint32_t> commandReadCursor{0};
  std::atomic<std::uint32_t> commandWriteCursor{0};
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
void DrainCommands(AudioCallbackState &state);
void ActivateScheduledSounds(AudioCallbackState &state,
                             long long bufferStartMicros, int sampleRate,
                             std::uint32_t frameCount);
void MixActiveSounds(AudioCallbackState &state, std::span<float> mixBuffer,
                     std::uint32_t frameCount, int outputChannels,
                     float bgmGain, float keysoundGain);

} // namespace audio::playback
