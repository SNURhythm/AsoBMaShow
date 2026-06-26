#define MINIAUDIO_IMPLEMENTATION
#include "../targets.h"
#include "AudioWrapper.h"
#include "../RAII.h"
#include <stdexcept>
#include <SDL2/SDL.h>
#include "decoder.h"
#include <sndfile.h>
#include <stdio.h>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <cstdint>
#if TARGET_OS_DESKTOP
#include <portaudio.h>
#endif

// Define IAudioBackend interface here
struct AudioWrapper::IAudioBackend {
  virtual ~IAudioBackend() = default;
  virtual void start() = 0;
  virtual void stop() = 0;
  virtual bool isStarted() const = 0;
  virtual int getSampleRate() const = 0;
};

AudioCallbackState::AudioCallbackState()
    : playingSounds(std::make_unique<PlayingSound[]>(kMaxActiveSounds)),
      scheduledSounds(
          std::make_unique<ScheduledSound[]>(kMaxScheduledSounds)),
      commandQueue(std::make_unique<AudioCommand[]>(kAudioCommandQueueSize)) {}

// Biquad Implementation
void Biquad::processStereo(float *buffer, size_t frameCount) {
  for (size_t i = 0; i < frameCount; ++i) {
    // Left Channel
    float inL = buffer[i * 2];
    float outL = inL * b0 + z1;
    z1 = inL * b1 + z2 - a1 * outL;
    z2 = inL * b2 - a2 * outL;
    buffer[i * 2] = outL;

    // Right Channel
    float inR = buffer[i * 2 + 1];
    float outR = inR * b0 + z1_r;
    z1_r = inR * b1 + z2_r - a1 * outR;
    z2_r = inR * b2 - a2 * outR;
    buffer[i * 2 + 1] = outR;
  }
}

void Biquad::setLowShelf(float fs, float f0, float gainDb, float Q) {
  float A = std::pow(10.0f, gainDb / 40.0f);
  float w0 = 2.0f * 3.14159265f * f0 / fs;
  float alpha = std::sin(w0) / (2.0f * Q);
  float cosw0 = std::cos(w0);

  float b0_tmp =
      A * ((A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * std::sqrt(A) * alpha);
  float b1_tmp = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
  float b2_tmp =
      A * ((A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * std::sqrt(A) * alpha);
  float a0_tmp = (A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * std::sqrt(A) * alpha;
  float a1_tmp = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
  float a2_tmp = (A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * std::sqrt(A) * alpha;

  b0 = b0_tmp / a0_tmp;
  b1 = b1_tmp / a0_tmp;
  b2 = b2_tmp / a0_tmp;
  a1 = a1_tmp / a0_tmp;
  a2 = a2_tmp / a0_tmp;
}

void Biquad::setHighShelf(float fs, float f0, float gainDb, float Q) {
  float A = std::pow(10.0f, gainDb / 40.0f);
  float w0 = 2.0f * 3.14159265f * f0 / fs;
  float alpha = std::sin(w0) / (2.0f * Q);
  float cosw0 = std::cos(w0);

  float b0_tmp =
      A * ((A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * std::sqrt(A) * alpha);
  float b1_tmp = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
  float b2_tmp =
      A * ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * std::sqrt(A) * alpha);
  float a0_tmp = (A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * std::sqrt(A) * alpha;
  float a1_tmp = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
  float a2_tmp = (A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * std::sqrt(A) * alpha;

  b0 = b0_tmp / a0_tmp;
  b1 = b1_tmp / a0_tmp;
  b2 = b2_tmp / a0_tmp;
  a1 = a1_tmp / a0_tmp;
  a2 = a2_tmp / a0_tmp;
}

// --- Reverb Helper Implementations ---

void CombFilter::init(size_t size) {
  buffer.resize(size, 0.0f);
  index = 0;
  filterStore = 0.0f;
}

float CombFilter::process(float input) {
  if (buffer.empty())
    return input;

  float output = buffer[index];
  filterStore = (output * (1.0f - damp)) + (filterStore * damp);

  buffer[index] = input + (filterStore * feedback);

  index++;
  if (index >= buffer.size())
    index = 0;

  return output;
}

void AllPassFilter::init(size_t size) {
  buffer.resize(size, 0.0f);
  index = 0;
}

float AllPassFilter::process(float input) {
  if (buffer.empty())
    return input;

  float bufOut = buffer[index];
  float output = -input + bufOut;

  buffer[index] = input + (bufOut * feedback);

  index++;
  if (index >= buffer.size())
    index = 0;

  return output;
}

// --- PlateReverb Implementation ---

void PlateReverb::init(int sampleRate) {
  float scale = (float)sampleRate / 44100.0f;

  // Input Diffusers (Decorrelators)
  inputDiffuser[0].init((size_t)(142 * scale));
  inputDiffuser[0].feedback = 0.75f;
  inputDiffuser[1].init((size_t)(107 * scale));
  inputDiffuser[1].feedback = 0.75f;

  // Tank Diffusers (Decay Diffusers)
  decayDiffuser[0].init((size_t)(379 * scale));
  decayDiffuser[0].feedback = 0.625f;
  decayDiffuser[1].init((size_t)(277 * scale));
  decayDiffuser[1].feedback = 0.625f;

  // Tank Delays (simulated via Comb with high feedback/damp control)
  tankComb[0].init((size_t)(4453 * scale));
  tankComb[0].feedback = 0.5f; // Initial feedback
  tankComb[1].init((size_t)(3720 * scale));
  tankComb[1].feedback = 0.5f;

  tankAllPass[0].init((size_t)(1800 * scale));
  tankAllPass[0].feedback = 0.5f;
  tankAllPass[1].init((size_t)(2656 * scale));
  tankAllPass[1].feedback = 0.5f;

  initialized = true;
  decay = 0.5f;
}

void PlateReverb::processStereo(float *buffer, size_t frameCount) {
  if (wet <= 0.001f || !initialized)
    return;

  // Use CombFilters as pure delays for the tank (no internal feedback)
  tankComb[0].feedback = 0.0f;
  tankComb[1].feedback = 0.0f;

  // Decay factor controls the cross-feedback gain directly
  float loopDecay = decay;

  for (size_t i = 0; i < frameCount; ++i) {
    float in = (buffer[i * 2] + buffer[i * 2 + 1]) * 0.5f;

    // Input Diffusion Chain
    float diff1 = inputDiffuser[0].process(in);
    float diff2 = inputDiffuser[1].process(diff1);

    // Dattorro-style Tank Cross-Coupling
    // We use the PREVIOUS output of the delay lines for cross-feedback.
    // filterStore contains the last output sample from the delay.
    float feedbackL = tankComb[1].filterStore * loopDecay;
    float feedbackR = tankComb[0].filterStore * loopDecay;

    // Left Tank Path
    float tankInL = diff2 + feedbackL;
    float apL = decayDiffuser[0].process(tankInL); // Decorrelate
    float delayL = tankComb[0].process(apL);       // Delay
    float tankL =
        tankAllPass[0].process(delayL); // More dispersion (output tap)

    // Right Tank Path
    float tankInR = diff2 + feedbackR;
    float apR = decayDiffuser[1].process(tankInR);
    float delayR = tankComb[1].process(apR);
    float tankR = tankAllPass[1].process(delayR);

    // Output Taps
    float outL = tankL;
    float outR = tankR;

    float wetScale = wet * 0.6f;
    buffer[i * 2] += outL * wetScale;
    buffer[i * 2 + 1] += outR * wetScale;
  }
}

void PlateReverb::setMix(float mix) { wet = mix; }

void PlateReverb::setDecay(float decayTime) {
  decay = decayTime;
  if (decay < 0.1f)
    decay = 0.1f;
  if (decay > 0.95f)
    decay = 0.95f; // Prevent explosion
}

// --- SoftKneeCompressor Implementation ---

void SoftKneeCompressor::init(int rate) { sampleRate = rate; }

void SoftKneeCompressor::processStereo(float *buffer, size_t frameCount) {
  if (!enabled)
    return;

  float alphaA = std::exp(-1.0f / (attack * sampleRate));
  float alphaR = std::exp(-1.0f / (release * sampleRate));
  float kneeHalf = kneeWidthDb / 2.0f;

  for (size_t i = 0; i < frameCount; ++i) {
    float l = buffer[i * 2];
    float r = buffer[i * 2 + 1];

    // RMS Detection (Approximate)
    float power = (l * l + r * r) * 0.5f;
    float inputLevel = std::sqrt(power);

    // Envelope follower
    if (inputLevel > envelope) {
      envelope = alphaA * envelope + (1.0f - alphaA) * inputLevel;
    } else {
      envelope = alphaR * envelope + (1.0f - alphaR) * inputLevel;
    }

    // Gain calculation
    float gain = 1.0f;
    float envDb = (envelope > 1e-6f) ? 20.0f * std::log10(envelope) : -120.0f;

    // Soft Knee Logic
    float slope = 1.0f - (1.0f / ratio);
    float over = envDb - thresholdDb;

    if (over > kneeHalf) {
      // Far above knee -> full compression
      float gainReduction = over * slope;
      gain = std::pow(10.0f, -gainReduction / 20.0f);
    } else if (over > -kneeHalf) {
      // In knee range -> interpolated compression
      float x = (over + kneeHalf) / kneeWidthDb; // 0..1 in knee
      float gainReduction =
          (0.5f * slope * x * x * kneeWidthDb); // Quadratic interpolation
      gain = std::pow(10.0f, -gainReduction / 20.0f);
    }

    // Apply gain
    buffer[i * 2] *= gain;
    buffer[i * 2 + 1] *= gain;
  }
}

void SoftKneeCompressor::setParams(float threshold, float r, float att,
                                   float rel) {
  thresholdDb = threshold;
  ratio = r;
  attack = att;
  release = rel;
  enabled = true;
}

namespace {
long long nowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

long long framesToMicros(int64_t frames, int sampleRate) {
  if (sampleRate <= 0) {
    sampleRate = 44100;
  }
  return static_cast<long long>((frames * 1000000LL) / sampleRate);
}

ma_uint32 outputOffsetForStartMicros(long long startMicros,
                                     long long bufferStartMicros,
                                     int sampleRate,
                                     ma_uint32 frameCount,
                                     bool &isDue) {
  isDue = true;
  if (startMicros <= bufferStartMicros) {
    return 0;
  }

  const long long deltaMicros = startMicros - bufferStartMicros;
  const uint64_t roundedFrame =
      (static_cast<uint64_t>(deltaMicros) * static_cast<uint64_t>(sampleRate) +
       500000ULL) /
      1000000ULL;
  if (roundedFrame >= frameCount) {
    isDue = false;
    return 0;
  }
  return static_cast<ma_uint32>(roundedFrame);
}

long long beginAudioClockBuffer(UserData *userData, ma_uint32 frameCount,
                                int sampleRate) {
  const int64_t startFrame =
      userData->audioClockFrameCursor->fetch_add(frameCount,
                                                 std::memory_order_acq_rel);
  const long long baseMicros =
      userData->audioClockBaseMicros->load(std::memory_order_acquire);
  const long long bufferStartMicros =
      baseMicros + framesToMicros(startFrame, sampleRate);
  const long long bufferEndMicros =
      baseMicros + framesToMicros(startFrame + frameCount, sampleRate);

  userData->audioClockAnchorMicros->store(bufferStartMicros,
                                          std::memory_order_release);
  userData->audioClockAnchorEndMicros->store(bufferEndMicros,
                                             std::memory_order_release);
  userData->audioClockAnchorWallMicros->store(nowMicros(),
                                              std::memory_order_release);
  return bufferStartMicros;
}

bool scheduledSoundLess(const ScheduledSound &lhs,
                        const ScheduledSound &rhs) {
  if (lhs.startMicros != rhs.startMicros) {
    return lhs.startMicros < rhs.startMicros;
  }
  return lhs.sequence < rhs.sequence;
}

void fillSilence(void *pOutput, ma_uint32 frameCount, int outputChannels) {
  std::fill_n((ma_int16 *)pOutput, frameCount * outputChannels, 0);
}

void removeActiveSoundAt(AudioCallbackState &state, size_t index) {
  SoundData *soundData = state.playingSounds[index].soundData;
  if (soundData) {
    soundData->playing = false;
  }
  --state.playingSoundCount;
  if (index < state.playingSoundCount) {
    state.playingSounds[index] = state.playingSounds[state.playingSoundCount];
  }
}

bool appendActiveSound(AudioCallbackState &state, SoundData *soundData,
                       ma_uint32 outputOffsetFrames, size_t startFrame = 0) {
  if (soundData == nullptr || startFrame >= soundData->resampledFrameCount ||
      state.playingSoundCount >= kMaxActiveSounds) {
    return false;
  }
  soundData->playing = true;
  state.playingSounds[state.playingSoundCount++] = {
      .soundData = soundData,
      .currentFrame = startFrame,
      .outputOffsetFrames = outputOffsetFrames,
  };
  return true;
}

bool insertScheduledSound(AudioCallbackState &state,
                          const ScheduledSound &scheduledSound) {
  if (scheduledSound.soundData == nullptr ||
      state.scheduledSoundCount >= kMaxScheduledSounds) {
    return false;
  }

  if (state.scheduledSoundCount == 0 ||
      !scheduledSoundLess(
          scheduledSound,
          state.scheduledSounds[state.scheduledSoundCount - 1])) {
    state.scheduledSounds[state.scheduledSoundCount++] = scheduledSound;
    return true;
  }

  size_t insertIndex = 0;
  while (insertIndex < state.scheduledSoundCount &&
         !scheduledSoundLess(scheduledSound,
                             state.scheduledSounds[insertIndex])) {
    ++insertIndex;
  }

  for (size_t i = state.scheduledSoundCount; i > insertIndex; --i) {
    state.scheduledSounds[i] = state.scheduledSounds[i - 1];
  }
  state.scheduledSounds[insertIndex] = scheduledSound;
  ++state.scheduledSoundCount;
  return true;
}

void clearCallbackSounds(AudioCallbackState &state) {
  for (size_t i = 0; i < state.playingSoundCount; ++i) {
    if (state.playingSounds[i].soundData) {
      state.playingSounds[i].soundData->playing = false;
    }
  }
  state.playingSoundCount = 0;
  state.scheduledSoundCount = 0;
}

void drainAudioCommands(AudioCallbackState &state) {
  uint32_t readCursor =
      state.commandReadCursor.load(std::memory_order_relaxed);
  const uint32_t writeCursor =
      state.commandWriteCursor.load(std::memory_order_acquire);

  while (readCursor != writeCursor) {
    const AudioCommand &command =
        state.commandQueue[readCursor % kAudioCommandQueueSize];
    switch (command.type) {
    case AudioCommandType::PlayNow:
      appendActiveSound(state, command.soundData, 0, command.startFrame);
      break;
    case AudioCommandType::Schedule:
      insertScheduledSound(state, {.soundData = command.soundData,
                                   .startMicros = command.startMicros,
                                   .sequence = command.sequence,
                                   .startFrame = command.startFrame});
      break;
    case AudioCommandType::StopAll:
      clearCallbackSounds(state);
      break;
    }
    ++readCursor;
  }

  state.commandReadCursor.store(readCursor, std::memory_order_release);
}

bool enqueueCallbackCommand(AudioCallbackState &state,
                            const AudioCommand &command) {
  const uint32_t readCursor =
      state.commandReadCursor.load(std::memory_order_acquire);
  const uint32_t writeCursor =
      state.commandWriteCursor.load(std::memory_order_relaxed);
  if (writeCursor - readCursor >= kAudioCommandQueueSize) {
    return false;
  }

  state.commandQueue[writeCursor % kAudioCommandQueueSize] = command;
  state.commandWriteCursor.store(writeCursor + 1, std::memory_order_release);
  return true;
}

void activateScheduledSounds(AudioCallbackState &state,
                             long long bufferStartMicros, int sampleRate,
                             ma_uint32 frameCount) {
  size_t scheduledSoundsToRemove = 0;
  for (; scheduledSoundsToRemove < state.scheduledSoundCount;
       ++scheduledSoundsToRemove) {
    const ScheduledSound &scheduledSound =
        state.scheduledSounds[scheduledSoundsToRemove];
    bool isDue = false;
    const ma_uint32 outputOffsetFrames = outputOffsetForStartMicros(
        scheduledSound.startMicros, bufferStartMicros, sampleRate, frameCount,
        isDue);
    if (!isDue) {
      break;
    }
    appendActiveSound(state, scheduledSound.soundData, outputOffsetFrames,
                      scheduledSound.startFrame);
  }

  if (scheduledSoundsToRemove == 0) {
    return;
  }

  const size_t remainingSounds =
      state.scheduledSoundCount - scheduledSoundsToRemove;
  for (size_t i = 0; i < remainingSounds; ++i) {
    state.scheduledSounds[i] =
        state.scheduledSounds[i + scheduledSoundsToRemove];
  }
  state.scheduledSoundCount = remainingSounds;
}

void mixActiveSounds(AudioCallbackState &state, float *mixBuffer,
                     ma_uint32 frameCount, int outputChannels) {
  constexpr float gain = 0.9f;
  size_t soundIndex = 0;
  while (soundIndex < state.playingSoundCount) {
    PlayingSound &playingSound = state.playingSounds[soundIndex];
    SoundData *soundData = playingSound.soundData;
    if (soundData == nullptr ||
        playingSound.currentFrame >= soundData->resampledFrameCount) {
      removeActiveSoundAt(state, soundIndex);
      continue;
    }

    const ma_uint32 outputOffsetFrames =
        std::min(playingSound.outputOffsetFrames, frameCount);
    ma_uint32 framesToRead = frameCount - outputOffsetFrames;
    ma_uint32 framesAvailable =
        soundData->resampledFrameCount - playingSound.currentFrame;
    if (framesToRead > framesAvailable) {
      framesToRead = framesAvailable;
    }

    const short *src = soundData->resampledData.data();
    const size_t currentFrame = playingSound.currentFrame;
    const int channels = soundData->channels;

    for (ma_uint32 frame = 0; frame < framesToRead; ++frame) {
      for (int channel = 0; channel < channels; ++channel) {
        const int outputChannel = channel % outputChannels;
        const float sample =
            src[(currentFrame + frame) * channels + channel] / 32768.0f;

        mixBuffer[(outputOffsetFrames + frame) * outputChannels +
                  outputChannel] += sample * gain;
      }
    }

    playingSound.currentFrame += framesToRead;
    playingSound.outputOffsetFrames = 0;
    if (playingSound.currentFrame >= soundData->resampledFrameCount) {
      removeActiveSoundAt(state, soundIndex);
      continue;
    }
    ++soundIndex;
  }
}

// Mixing logic extracted to be backend-agnostic
void mixAudio(void *pOutput, ma_uint32 frameCount, int outputChannels,
              UserData *userData) {
  if (userData == nullptr || userData->callbackState == nullptr) {
    return;
  }

  AudioCallbackState &state = *userData->callbackState;
  drainAudioCommands(state);

  if (!userData->stopwatch->isRunning()) {
    fillSilence(pOutput, frameCount, outputChannels);
    return;
  }

  int sampleRate = userData->sampleRate ? userData->sampleRate->load() : 44100;
  if (sampleRate <= 0) {
    sampleRate = 44100;
  }

  const long long bufferStartMicros =
      beginAudioClockBuffer(userData, frameCount, sampleRate);
  activateScheduledSounds(state, bufferStartMicros, sampleRate, frameCount);

  if (state.playingSoundCount == 0) {
    fillSilence(pOutput, frameCount, outputChannels);
    return;
  }

  // Resize mix buffer if necessary
  size_t requiredSamples = frameCount * outputChannels;
  if (userData->mixBuffer->size() < requiredSamples) {
    userData->mixBuffer->resize(requiredSamples);
  }

  // Clear mix buffer
  std::fill(userData->mixBuffer->begin(),
            userData->mixBuffer->begin() + requiredSamples, 0.0f);
  float *mixBuffer = userData->mixBuffer->data();

  mixActiveSounds(state, mixBuffer, frameCount, outputChannels);

  // Apply Effects
  if (userData->bassFilter) {
    userData->bassFilter->processStereo(mixBuffer, frameCount);
  }
  if (userData->trebleFilter) {
    userData->trebleFilter->processStereo(mixBuffer, frameCount);
  }

  if (userData->reverb && userData->reverb->initialized) {
    userData->reverb->processStereo(mixBuffer, frameCount);
  }

  if (userData->compressor && userData->compressor->enabled) {
    userData->compressor->processStereo(mixBuffer, frameCount);
  }

  // Convert back to int16
  ma_int16 *outPtr = (ma_int16 *)pOutput;
  for (size_t i = 0; i < requiredSamples; ++i) {
    float sample = mixBuffer[i];

    // Hard clip
    if (sample > 1.0f)
      sample = 1.0f;
    if (sample < -1.0f)
      sample = -1.0f;

    outPtr[i] = (ma_int16)(sample * 32767.0f);
  }
}

ma_result initMiniaudioDevice(const ma_device_config *deviceConfig,
                              ma_device *device) {
#if TARGET_OS_IPHONE
  ma_context_config contextConfig = ma_context_config_init();
  contextConfig.coreaudio.sessionCategory = ma_ios_session_category_ambient;
  contextConfig.coreaudio.sessionCategoryOptions =
      ma_ios_session_category_option_mix_with_others;
  return ma_device_init_ex(nullptr, 0, &contextConfig, deviceConfig, device);
#else
  return ma_device_init(nullptr, deviceConfig, device);
#endif
}
} // namespace

// Miniaudio Backend Implementation
class MiniaudioBackend : public AudioWrapper::IAudioBackend {
public:
  MiniaudioBackend(UserData *userData) {
    ma_device_config deviceConfig =
        ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = ma_format_s16;
    deviceConfig.playback.channels = 2;
    deviceConfig.sampleRate = 0; // Use native sample rate
    deviceConfig.dataCallback = dataCallback;
    deviceConfig.pUserData = userData;

    if (initMiniaudioDevice(&deviceConfig, &device) != MA_SUCCESS) {
      throw std::runtime_error(
          "Failed to initialize miniaudio playback device.");
    }
    SDL_Log("[Miniaudio] Initialized with sample rate: %d", device.sampleRate);
  }

  ~MiniaudioBackend() override { ma_device_uninit(&device); }

  void start() override {
    if (!ma_device_is_started(&device)) {
      ma_device_start(&device);
      SDL_Log("[Miniaudio] Started playback device.");
    }
  }

  void stop() override {
    if (ma_device_is_started(&device)) {
      ma_device_stop(&device);
      SDL_Log("[Miniaudio] Stopped playback device.");
    }
  }

  bool isStarted() const override { return ma_device_is_started(&device); }

  int getSampleRate() const override { return device.sampleRate; }

private:
  ma_device device;

  static void dataCallback(ma_device *pDevice, void *pOutput,
                           const void *pInput, ma_uint32 frameCount) {
    auto *userData = (UserData *)pDevice->pUserData;
    // Miniaudio output matches the logic expected by mixAudio (int16 buffer)
    mixAudio(pOutput, frameCount, pDevice->playback.channels, userData);
  }
};

// PortAudio Backend Implementation
#if TARGET_OS_DESKTOP
class PortAudioBackend : public AudioWrapper::IAudioBackend {
public:
  PortAudioBackend(UserData *userData)
      : userData(userData), stream(nullptr), sampleRate(44100) {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
      SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "[PortAudio] init error: %s",
                   Pa_GetErrorText(err));
      throw std::runtime_error("Failed to initialize PortAudio");
    }
    auto terminateOnFailure = makeScopeExit([]() { Pa_Terminate(); });

    PaStreamParameters outputParameters;
    outputParameters.device = Pa_GetDefaultOutputDevice(); // Default

// Try to find ASIO device on Windows
#ifdef TARGET_OS_WINDOWS
    int numDevices = Pa_GetDeviceCount();
    for (int i = 0; i < numDevices; ++i) {
      const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
      const PaHostApiInfo *hostApi = Pa_GetHostApiInfo(info->hostApi);
      if (hostApi && hostApi->type == paASIO) {
        outputParameters.device = i;
        SDL_Log("Found ASIO device: %s", info->name);
        break;
      }
    }
#endif

    if (outputParameters.device == paNoDevice) {
      throw std::runtime_error("No default output device.");
    }

    const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(outputParameters.device);
    sampleRate = (int)deviceInfo->defaultSampleRate;

    outputParameters.channelCount = 2; // Stereo
    outputParameters.sampleFormat = paInt16;
    outputParameters.suggestedLatency = deviceInfo->defaultLowOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = nullptr;

    err = Pa_OpenStream(&stream,
                        nullptr, // No input
                        &outputParameters, (double)sampleRate,
                        paFramesPerBufferUnspecified,
                        paClipOff, // We clamp manually
                        paCallback, this);

    if (err != paNoError) {
      if (stream != nullptr) {
        Pa_CloseStream(stream);
        stream = nullptr;
      }
      SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "[PortAudio] OpenStream error: %s",
                   Pa_GetErrorText(err));
      throw std::runtime_error("Failed to open audio stream");
    }
    SDL_Log("[PortAudio] Output device: %s", deviceInfo->name);
    SDL_Log("[PortAudio] Initialized with sample rate: %d", sampleRate);
    terminateOnFailure.dismiss();
  }

  ~PortAudioBackend() override {
    if (stream) {
      Pa_CloseStream(stream);
    }
    Pa_Terminate();
  }

  void start() override {
    if (stream && Pa_IsStreamStopped(stream)) {
      Pa_StartStream(stream);
      SDL_Log("[PortAudio] Started playback stream.");
    }
  }

  void stop() override {
    if (stream && !Pa_IsStreamStopped(stream)) {
      Pa_StopStream(stream);
      SDL_Log("[PortAudio] Stopped playback stream.");
    }
  }

  bool isStarted() const override {
    return stream && !Pa_IsStreamStopped(stream);
  }

  int getSampleRate() const override { return sampleRate; }

private:
  UserData *userData;
  PaStream *stream;
  int sampleRate;

  static int paCallback(const void *inputBuffer, void *outputBuffer,
                        unsigned long framesPerBuffer,
                        const PaStreamCallbackTimeInfo *timeInfo,
                        PaStreamCallbackFlags statusFlags, void *userData) {
    auto *backend = (PortAudioBackend *)userData;
    // PortAudio requesting paInt16, so outputBuffer is int16*
    mixAudio(outputBuffer, (ma_uint32)framesPerBuffer, 2, backend->userData);
    return paContinue;
  }
};
#endif

// AudioWrapper Implementation

AudioWrapper::AudioWrapper(Stopwatch *stopwatch) : stopwatch(stopwatch) {
  userData.callbackState = &callbackState;
  userData.sampleRate = &currentSampleRate;
  userData.audioClockBaseMicros = &audioClockBaseMicros;
  userData.audioClockFrameCursor = &audioClockFrameCursor;
  userData.audioClockAnchorMicros = &audioClockAnchorMicros;
  userData.audioClockAnchorWallMicros = &audioClockAnchorWallMicros;
  userData.audioClockAnchorEndMicros = &audioClockAnchorEndMicros;
  userData.stopwatch = stopwatch;
  userData.mixBuffer = &mixBuffer;
  userData.bassFilter = &bassFilter;
  userData.trebleFilter = &trebleFilter;
  userData.reverb = &reverb;
  userData.compressor = &compressor;

#if TARGET_OS_DESKTOP
  // Default to PortAudio on Desktop
  try {
    backend = std::make_unique<PortAudioBackend>(&userData);
    SDL_Log("Initialized PortAudio backend.");
  } catch (const std::exception &e) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                 "Failed to initialize PortAudio backend: %s. Falling back to "
                 "Miniaudio.",
                 e.what());
    backend = std::make_unique<MiniaudioBackend>(&userData);
  }
#else
  // Default to Miniaudio on other platforms
  backend = std::make_unique<MiniaudioBackend>(&userData);
  SDL_Log("Initialized Miniaudio backend.");
#endif

  updateCurrentSampleRate();
  startDevice();

  // //
  // setBassBoost(3.0f);   // Warmth
  // setTrebleBoost(3.0f); // Clarity

  // // Plate Reverb: Richer, denser tail
  // reverb.setDecay(0.6f); // ~1.5s decay time
  // setReverbMix(0.4f);    // Wets the mix without drowning transients

  // // Soft Knee Compressor: Transparent glue
  // // Threshold -8dB (RMS), Ratio 2.5:1, Attack 30ms (let transients pass),
  // // Release 150ms
  // compressor.setParams(-8.0f, 2.5f, 0.03f, 0.15f);
}

AudioWrapper::~AudioWrapper() {
  unloadSounds();
  // Backend destroyed via unique_ptr
}

int AudioWrapper::IAudioBackend::getSampleRate() const {
  return 44100;
} // Default if virtual fails

void AudioWrapper::updateCurrentSampleRate() {
  int rate = backend ? backend->getSampleRate() : 44100;
  if (rate <= 0) {
    rate = 44100;
  }
  currentSampleRate.store(rate, std::memory_order_release);
}

long long AudioWrapper::getTimeMicros() const {
  const long long anchorMicros =
      audioClockAnchorMicros.load(std::memory_order_acquire);
  const long long anchorWallMicros =
      audioClockAnchorWallMicros.load(std::memory_order_acquire);
  const long long anchorEndMicros =
      audioClockAnchorEndMicros.load(std::memory_order_acquire);

  if (anchorWallMicros <= 0 || !stopwatch->isRunning()) {
    return anchorMicros;
  }

  long long interpolatedMicros = anchorMicros + nowMicros() - anchorWallMicros;
  if (anchorEndMicros >= anchorMicros &&
      interpolatedMicros > anchorEndMicros) {
    interpolatedMicros = anchorEndMicros;
  }
  if (interpolatedMicros < anchorMicros) {
    interpolatedMicros = anchorMicros;
  }
  return interpolatedMicros;
}

void AudioWrapper::seekClock(long long micros) {
  std::lock_guard<std::mutex> lock(audioCommandMutex);
  const long long wallMicros = nowMicros();
  audioClockBaseMicros.store(micros, std::memory_order_release);
  audioClockFrameCursor.store(0, std::memory_order_release);
  audioClockAnchorMicros.store(micros, std::memory_order_release);
  audioClockAnchorEndMicros.store(micros, std::memory_order_release);
  audioClockAnchorWallMicros.store(wallMicros, std::memory_order_release);
}

bool AudioWrapper::loadSound(const path_t &path,
                             std::atomic<bool> &isCancelled) {
  {
    std::lock_guard<std::mutex> lock(soundDataListMutex);
    if (soundDataIndexMap.contains(path)) {
      return true;
    }
  }

  std::vector<short> pcmData;
  SF_INFO sfInfo;
  bool result = decodeAudioToPCM(path, pcmData, sfInfo, isCancelled);
  if (!result) {
    SDL_Log("Failed to decode audio file %s", path_t_to_utf8(path).c_str());
    return false;
  }
  return loadDecodedSound(path, std::move(pcmData), sfInfo.channels,
                          sfInfo.samplerate, isCancelled);
}

bool AudioWrapper::loadSoundFromMemory(
    const path_t &path, const std::vector<unsigned char> &bytes,
    std::atomic<bool> &isCancelled) {
  {
    std::lock_guard<std::mutex> lock(soundDataListMutex);
    if (soundDataIndexMap.contains(path)) {
      return true;
    }
  }

  std::vector<short> pcmData;
  SF_INFO sfInfo;
  bool result = decodeAudioBytesToPCM(path, bytes, pcmData, sfInfo,
                                      isCancelled);
  if (!result) {
    SDL_Log("Failed to decode audio file %s", path_t_to_utf8(path).c_str());
    return false;
  }
  return loadDecodedSound(path, std::move(pcmData), sfInfo.channels,
                          sfInfo.samplerate, isCancelled);
}

bool AudioWrapper::loadDecodedSound(const path_t &path,
                                    std::vector<short> pcmData, int channels,
                                    int sampleRate,
                                    std::atomic<bool> &isCancelled) {
  if (isCancelled) {
    return false;
  }

  auto soundData = std::make_shared<SoundData>();

  soundData->currentFrame = 0;
  soundData->channels = channels;
  soundData->originalSampleRate = sampleRate;
  soundData->playing = false;

  updateCurrentSampleRate();
  int targetSampleRate = currentSampleRate.load(std::memory_order_acquire);
  SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION,
                 "Target sample rate: %d, File sample rate: %d",
                 targetSampleRate, sampleRate);

  if (targetSampleRate == sampleRate) {
    // Optimization: Skip resampling
    soundData->isResampled = false;
    soundData->resampledData = std::move(pcmData);
    soundData->resampledFrameCount =
        soundData->resampledData.size() / soundData->channels;
    SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION,
                   "Loaded sound without resampling (Rate: %d)",
                   targetSampleRate);
  } else {
    // Initialize the resampler
    SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION,
                   "Resampling audio data from %d Hz to %d Hz", sampleRate,
                   targetSampleRate);
    ma_resampler_config resamplerConfig = ma_resampler_config_init(
        ma_format_s16, channels, sampleRate, targetSampleRate,
        ma_resample_algorithm_linear);
    if (ma_resampler_init(&resamplerConfig, nullptr, &soundData->resampler) !=
        MA_SUCCESS) {
      SDL_Log("Failed to initialize resampler.");
      return false;
    }
    soundData->resamplerInitialized = true;
    soundData->isResampled = true;
    if (isCancelled)
      return false;

    // Resample the audio data to target rate

    ma_uint64 resampledFrameCount =
        (ma_uint64)((double)pcmData.size() / channels * targetSampleRate /
                    sampleRate);
    soundData->resampledData.resize(resampledFrameCount * channels);
    ma_uint64 size = (ma_uint64)pcmData.size();
    if (isCancelled)
      return false;
    ma_resampler_process_pcm_frames(&soundData->resampler, pcmData.data(),
                                    &size, soundData->resampledData.data(),
                                    &resampledFrameCount);
    if (isCancelled)
      return false;
    soundData->resampledFrameCount = resampledFrameCount;
  }

  {
    std::lock_guard<std::mutex> lock(soundDataListMutex);
    if (soundDataIndexMap.contains(path)) {
      return true;
    }
    soundDataIndexMap[path] = soundDataList.size();
    soundDataList.push_back(soundData);
  }
  return true;
}

void AudioWrapper::preloadSounds(const std::vector<path_t> &paths,
                                 std::atomic<bool> &isCancelled) {
  for (const auto &path : paths) {
    loadSound(path, isCancelled);
  }
}

bool AudioWrapper::appendScheduledSound(SoundData *soundData,
                                        long long startMicros,
                                        uint64_t sequence,
                                        size_t startFrame) {
  return insertScheduledSound(callbackState, {.soundData = soundData,
                                              .startMicros = startMicros,
                                              .sequence = sequence,
                                              .startFrame = startFrame});
}

void AudioWrapper::clearCallbackState() {
  clearCallbackSounds(callbackState);
  callbackState.commandReadCursor.store(0, std::memory_order_release);
  callbackState.commandWriteCursor.store(0, std::memory_order_release);
}

bool AudioWrapper::playSound(const path_t &path, long long startOffsetMicros) {
  std::lock_guard<std::mutex> lock(soundDataListMutex);

  const auto indexIt = soundDataIndexMap.find(path);
  if (indexIt == soundDataIndexMap.end()) {
    SDL_Log("Sound not found: %s", path_t_to_utf8(path).c_str());
    return false;
  }

  auto &soundData = soundDataList[indexIt->second];
  const long long clampedOffsetMicros = std::max(0LL, startOffsetMicros);
  const size_t startFrame = static_cast<size_t>(
      std::min<long long>(
          static_cast<long long>(soundData->resampledFrameCount),
          clampedOffsetMicros *
              static_cast<long long>(
                  currentSampleRate.load(std::memory_order_acquire)) /
              1000000LL));
  if (startFrame >= soundData->resampledFrameCount) {
    return false;
  }

  bool shouldStartBackend = false;
  {
    std::lock_guard<std::mutex> commandLock(audioCommandMutex);
    if (backend && !backend->isStarted()) {
      if (!appendActiveSound(callbackState, soundData.get(), 0, startFrame)) {
        SDL_Log("Too many active sounds; dropping %s",
                path_t_to_utf8(path).c_str());
        return false;
      }
      shouldStartBackend = true;
    } else if (!enqueueCallbackCommand(
                   callbackState, {.type = AudioCommandType::PlayNow,
                                   .soundData = soundData.get(),
                                   .startFrame = startFrame})) {
      SDL_Log("Audio command queue full; dropping %s",
              path_t_to_utf8(path).c_str());
      return false;
    }
  }

  if (shouldStartBackend) {
    backend->start();
  }

  return true;
}

std::optional<long long>
AudioWrapper::getSoundDurationMicros(const path_t &path) const {
  std::lock_guard<std::mutex> lock(soundDataListMutex);

  const auto indexIt = soundDataIndexMap.find(path);
  if (indexIt == soundDataIndexMap.end()) {
    return std::nullopt;
  }

  const auto &soundData = soundDataList[indexIt->second];
  const int sampleRate = currentSampleRate.load(std::memory_order_acquire);
  if (soundData == nullptr || sampleRate <= 0) {
    return std::nullopt;
  }
  return static_cast<long long>(
      static_cast<double>(soundData->resampledFrameCount) * 1000000.0 /
      static_cast<double>(sampleRate));
}

bool AudioWrapper::scheduleSound(const path_t &path, long long startMicros) {
  std::lock_guard<std::mutex> lock(soundDataListMutex);

  const auto indexIt = soundDataIndexMap.find(path);
  if (indexIt == soundDataIndexMap.end()) {
    SDL_Log("Sound not found: %s", path_t_to_utf8(path).c_str());
    return false;
  }

  auto &soundData = soundDataList[indexIt->second];
  const uint64_t sequence =
      scheduledSoundSequence.fetch_add(1, std::memory_order_acq_rel);

  {
    std::lock_guard<std::mutex> commandLock(audioCommandMutex);
    if (backend && !backend->isStarted()) {
      if (!appendScheduledSound(soundData.get(), startMicros, sequence)) {
        SDL_Log("Too many scheduled sounds; dropping %s",
                path_t_to_utf8(path).c_str());
        return false;
      }
    } else if (!enqueueCallbackCommand(
                   callbackState, {.type = AudioCommandType::Schedule,
                                   .soundData = soundData.get(),
                                   .startMicros = startMicros,
                                   .sequence = sequence})) {
      SDL_Log("Audio command queue full; dropping scheduled %s",
              path_t_to_utf8(path).c_str());
      return false;
    }
  }

  return true;
}

void AudioWrapper::startDevice() {
  if (backend) {
    updateCurrentSampleRate();
    std::lock_guard<std::mutex> lock(audioCommandMutex);
    backend->start();
  }
}

void AudioWrapper::stopSounds() {
  if (backend) {
    backend->stop();
  }
  std::lock_guard<std::mutex> lock(audioCommandMutex);
  clearCallbackState();
}

void AudioWrapper::unloadSound(const path_t &path) {
  stopSounds();

  std::lock_guard<std::mutex> lock(soundDataListMutex);
  if (const auto indexIt = soundDataIndexMap.find(path);
      indexIt != soundDataIndexMap.end()) {
    const size_t index = indexIt->second;
    auto &soundData = soundDataList[index];

    soundData->releaseResampler();

    soundDataList.erase(soundDataList.begin() + index);
    soundDataIndexMap.erase(indexIt);

    // Update indices in the map
    for (auto &entry : soundDataIndexMap) {
      if (entry.second > index) {
        entry.second--;
      }
    }
  }
}

void AudioWrapper::unloadSounds() {
  stopSounds();
  {
    std::lock_guard<std::mutex> lock(soundDataListMutex);
    for (auto &soundData : soundDataList) {
      soundData->releaseResampler();
    }
    soundDataList.clear();
    soundDataIndexMap.clear();
  }
}

void AudioWrapper::setBassBoost(float db) {
  std::lock_guard<std::mutex> lock(soundDataListMutex); // Protect filter coeffs

  // Get current sample rate from backend or default
  int rate = backend ? backend->getSampleRate() : 44100;
  if (rate == 0)
    rate = 44100;

  // Low Shelf at 100Hz
  bassFilter.setLowShelf((float)rate, 100.0f, db);
}

void AudioWrapper::setTrebleBoost(float db) {
  std::lock_guard<std::mutex> lock(soundDataListMutex); // Protect filter coeffs

  // Get current sample rate
  int rate = backend ? backend->getSampleRate() : 44100;
  if (rate == 0)
    rate = 44100;

  // High Shelf at 3000Hz (or user preference 8-10kHz? 3kHz is mid-high, let's
  // say 4kHz)
  trebleFilter.setHighShelf((float)rate, 4000.0f, db);
}

void AudioWrapper::setReverbMix(float mix) {
  std::lock_guard<std::mutex> lock(soundDataListMutex);

  int rate = backend ? backend->getSampleRate() : 44100;
  if (rate == 0)
    rate = 44100;

  if (!reverb.initialized) {
    reverb.init(rate);
  }
  reverb.setMix(mix);
}

void AudioWrapper::setCompressor(float threshold, float ratio) {
  std::lock_guard<std::mutex> lock(soundDataListMutex);

  int rate = backend ? backend->getSampleRate() : 44100;
  if (rate == 0)
    rate = 44100;

  if (!compressor.enabled && ratio > 1.0f) {
    compressor.init(rate);
  }

  if (ratio <= 1.0f) {
    compressor.enabled = false;
  } else {
    compressor.setParams(threshold, ratio, 0.01f,
                         0.1f); // Default attack/release
  }
}
