#pragma once

#include <miniaudio.h>
#include <memory>
#include <string>
#include <vector>
#include "../path.h"
#include "../utils/Stopwatch.h"
#include <mutex>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>

// Simple Biquad Filter
struct Biquad {
  float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
  float z1 = 0.0f, z2 = 0.0f;
  float z1_r = 0.0f, z2_r = 0.0f; // For stereo (right channel)

  void processStereo(float *buffer, size_t frameCount);
  void setLowShelf(float fs, float f0, float gainDb, float Q = 0.707f);
  void setHighShelf(float fs, float f0, float gainDb, float Q = 0.707f);
};

// Reverb Helper Structures
struct CombFilter {
  std::vector<float> buffer;
  size_t index = 0;
  float feedback = 0.8f;
  float damp = 0.2f;
  float filterStore = 0.0f;

  void init(size_t size);
  float process(float input);
};

struct AllPassFilter {
  std::vector<float> buffer;
  size_t index = 0;
  float feedback = 0.5f;

  void init(size_t size);
  float process(float input);
};

struct PlateReverb {
  // Dattorro Plate Reverb constants
  AllPassFilter inputDiffuser[2];
  AllPassFilter decayDiffuser[2]; // In loop
  // Delays for tank
  // We need modulated delay lines for a true Dattorro, but for now we'll use
  // fixed delays to avoid complexity of LFO implementation in this step.
  // Enhanced structure: Pre-delay -> Input Diffusers -> Tank (Loops with
  // AllPass + Delay + LowPass)

  // Tank components
  CombFilter
      tankComb[2]; // Recycling simple combs as delays with feedback/damping
  AllPassFilter tankAllPass[2];

  float wet = 0.0f;
  float decay = 0.5f; // Reverb time
  bool initialized = false;

  void init(int sampleRate);
  void processStereo(float *buffer, size_t frameCount);
  void setMix(float mix);
  void setDecay(float decayTime);
};

struct SoftKneeCompressor {
  float thresholdDb = 0.0f;
  float ratio = 1.0f;
  float attack = 0.01f;
  float release = 0.1f;
  float envelope = 0.0f;
  float kneeWidthDb = 12.0f; // Soft knee width
  int sampleRate = 44100;
  bool enabled = false;

  void init(int rate);
  void processStereo(float *buffer, size_t frameCount);
  void setParams(float threshold, float ratio, float attack, float release);
};

// Custom data structure to hold PCM data and playback state
struct SoundData {

  size_t currentFrame;
  int channels;
  int originalSampleRate;
  bool playing;
  bool isResampled;
  ma_resampler resampler;
  std::vector<short> resampledData;
  size_t resampledFrameCount;
};

struct PlayingSound {
  SoundData *soundData;
  size_t currentFrame;
  ma_uint32 outputOffsetFrames;
};

struct ScheduledSound {
  SoundData *soundData;
  long long startMicros;
  uint64_t sequence;
  size_t startFrame;
};

enum class AudioCommandType : uint8_t { PlayNow, Schedule, StopAll };

struct AudioCommand {
  AudioCommandType type = AudioCommandType::StopAll;
  SoundData *soundData = nullptr;
  long long startMicros = 0;
  uint64_t sequence = 0;
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
  std::atomic<uint32_t> commandReadCursor{0};
  std::atomic<uint32_t> commandWriteCursor{0};
};

struct UserData {
  Stopwatch *stopwatch;
  AudioCallbackState *callbackState;
  std::atomic<int> *sampleRate;
  std::atomic<long long> *audioClockBaseMicros;
  std::atomic<int64_t> *audioClockFrameCursor;
  std::atomic<long long> *audioClockAnchorMicros;
  std::atomic<long long> *audioClockAnchorWallMicros;
  std::atomic<long long> *audioClockAnchorEndMicros;
  std::vector<float> *mixBuffer;
  Biquad *bassFilter;
  Biquad *trebleFilter;
  PlateReverb *reverb;
  SoftKneeCompressor *compressor;
};
class AudioWrapper {
public:
  AudioWrapper(Stopwatch *stopwatch);
  ~AudioWrapper();
  bool loadSound(const path_t &path, std::atomic<bool> &isCancelled);
  bool loadSoundFromMemory(const path_t &path,
                           const std::vector<unsigned char> &bytes,
                           std::atomic<bool> &isCancelled);
  void preloadSounds(const std::vector<path_t> &paths,
                     std::atomic<bool> &isCancelled);
  bool playSound(const path_t &path, long long startOffsetMicros = 0);
  bool scheduleSound(const path_t &path, long long startMicros);
  std::optional<long long> getSoundDurationMicros(const path_t &path) const;
  long long getTimeMicros() const;
  void seekClock(long long micros);
  void startDevice();
  void stopSounds();
  void unloadSound(const path_t &path);

  void setBassBoost(float db);
  void setTrebleBoost(float db);
  void setReverbMix(float mix);
  void setCompressor(float threshold, float ratio);

  void unloadSounds();

  struct IAudioBackend; // Forward declaration

private:
  std::unique_ptr<IAudioBackend> backend;

  std::vector<std::shared_ptr<SoundData>> soundDataList;
  AudioCallbackState callbackState;
  std::atomic<uint64_t> scheduledSoundSequence{0};
  std::mutex audioCommandMutex;
  std::unordered_map<path_t, size_t>
      soundDataIndexMap; // Map to store index of SoundData in soundDataList
  mutable std::mutex soundDataListMutex;
  std::vector<float> mixBuffer;
  Biquad bassFilter;
  Biquad trebleFilter;
  PlateReverb reverb;
  SoftKneeCompressor compressor;
  std::atomic<int> currentSampleRate{44100};
  std::atomic<long long> audioClockBaseMicros{0};
  std::atomic<int64_t> audioClockFrameCursor{0};
  std::atomic<long long> audioClockAnchorMicros{0};
  std::atomic<long long> audioClockAnchorWallMicros{0};
  std::atomic<long long> audioClockAnchorEndMicros{0};

  UserData userData;
  Stopwatch *stopwatch;

  void updateCurrentSampleRate();
  bool loadDecodedSound(const path_t &path, std::vector<short> pcmData,
                        int channels, int sampleRate,
                        std::atomic<bool> &isCancelled);
  bool appendScheduledSound(SoundData *soundData, long long startMicros,
                            uint64_t sequence, size_t startFrame = 0);
  void clearCallbackState();
};
