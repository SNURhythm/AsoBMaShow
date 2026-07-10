#pragma once

#include "AudioMix.h"
#include "AudioDeviceManager.h"
#include "../settings/AudioVideoSettings.h"

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

struct UserData {
  Stopwatch *stopwatch;
  AudioCallbackState *callbackState;
  std::atomic<int> *sampleRate;
  std::atomic<long long> *audioClockBaseMicros;
  std::atomic<int64_t> *audioClockFrameCursor;
  std::atomic<long long> *audioClockAnchorMicros;
  std::atomic<long long> *audioClockAnchorWallMicros;
  std::atomic<long long> *audioClockAnchorEndMicros;
  std::atomic<float> *bgmGain;
  std::atomic<float> *keysoundGain;
  std::vector<float> *mixBuffer;
  Biquad *bassFilter;
  Biquad *trebleFilter;
  PlateReverb *reverb;
  SoftKneeCompressor *compressor;
};
class AudioWrapper : public audio::IAudioRuntime {
public:
  AudioWrapper(Stopwatch *stopwatch);
  AudioWrapper(Stopwatch *stopwatch,
               std::unique_ptr<audio::IBackendFactory> injectedFactory);
  AudioWrapper(
      Stopwatch *stopwatch,
      std::unique_ptr<audio::playback::IBackendLifecycle> injectedBackend);
  ~AudioWrapper();
  bool loadSound(const path_t &path, std::atomic<bool> &isCancelled);
  bool loadSoundFromMemory(const path_t &path,
                           const std::vector<unsigned char> &bytes,
                           std::atomic<bool> &isCancelled);
  bool loadGeneratedSound(const path_t &path, std::vector<short> pcmData,
                          int channels, int sampleRate);
  void preloadSounds(const std::vector<path_t> &paths,
                     std::atomic<bool> &isCancelled);
  bool playSound(const path_t &path, audio::Bus bus,
                 long long startOffsetMicros = 0);
  bool scheduleSound(const path_t &path, audio::Bus bus, long long startMicros);
  std::optional<long long> getSoundDurationMicros(const path_t &path) const;
  long long getTimeMicros() const;
  void seekClock(long long micros);
  audio::playback::BackendOperationResult startDevice();
  audio::playback::BackendOperationResult stopSounds();
  audio::playback::BackendOperationResult unloadSound(const path_t &path);
  audio::playback::BackendOperationResult
  pruneSounds(const std::vector<path_t> &paths);

  void setBassBoost(float db);
  void setTrebleBoost(float db);
  void setReverbMix(float mix);
  void setCompressor(float threshold, float ratio);
  [[nodiscard]] audio::Capabilities capabilities() const override;
  [[nodiscard]] audio::RuntimeState runtimeState() const override;
  bool restart(const audio::StreamRequest &request,
               std::string &errorMessage) override;
  bool restore(const audio::RuntimeState &previous,
               std::string &errorMessage) override;
  void setVolumes(const audio::Volumes &volumes) override;
  void setVolumes(const player_settings::AudioSettings &settings);

  audio::playback::BackendOperationResult unloadSounds();

private:
  std::unique_ptr<audio::IBackendFactory> backendFactory;
  std::unique_ptr<audio::playback::IBackendLifecycle> backend;

  std::vector<std::shared_ptr<SoundData>> soundDataList;
  AudioCallbackState callbackState;
  std::atomic<uint64_t> scheduledSoundSequence{0};
  mutable std::mutex deviceLifecycleMutex;
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
  std::atomic<audio::playback::BackendRunState> backendState{
      audio::playback::BackendRunState::Unknown};
  std::atomic<long long> audioClockBaseMicros{0};
  std::atomic<int64_t> audioClockFrameCursor{0};
  std::atomic<long long> audioClockAnchorMicros{0};
  std::atomic<long long> audioClockAnchorWallMicros{0};
  std::atomic<long long> audioClockAnchorEndMicros{0};
  std::atomic<float> bgmGain{1.0f};
  std::atomic<float> keysoundGain{1.0f};
  audio::RuntimeState runtimeState_;

  UserData userData;
  Stopwatch *stopwatch;

  bool loadDecodedSound(const path_t &path, std::vector<short> pcmData,
                        int channels, int sampleRate,
                        std::atomic<bool> &isCancelled);
  bool appendScheduledSound(SoundData *soundData, long long startMicros,
                            uint64_t sequence, audio::Bus bus,
                            size_t startFrame = 0);
  void initializeUserData();
  void startBackendAfterConstruction();
  audio::playback::BackendOperationResult
  stopSoundsWithLifecycleAndCommandLocked();
  void clearCallbackState();
};
