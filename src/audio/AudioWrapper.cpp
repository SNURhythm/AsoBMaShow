#define MINIAUDIO_IMPLEMENTATION
#include "AudioWrapper.h"
#include <stdexcept>
#include <SDL2/SDL.h>
#include "decoder.h"
#include <sndfile.h>
#include <stdio.h>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>

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

long long framesToChartMicros(int64_t frames, int sampleRate,
                              int playbackRatePercent) {
  if (sampleRate <= 0) {
    sampleRate = 44100;
  }
  const int ratePercent = playbackRatePercent > 0 ? playbackRatePercent : 100;
#if defined(__SIZEOF_INT128__)
  const __int128 result =
      static_cast<__int128>(frames) * 1000000 * ratePercent / sampleRate / 100;
  return static_cast<long long>(std::clamp(
      result, static_cast<__int128>(std::numeric_limits<long long>::min()),
      static_cast<__int128>(std::numeric_limits<long long>::max())));
#else
  const long double result = static_cast<long double>(frames) * 1000000.0L *
                             ratePercent / sampleRate / 100.0L;
  return static_cast<long long>(std::clamp(
      result, static_cast<long double>(std::numeric_limits<long long>::min()),
      static_cast<long double>(std::numeric_limits<long long>::max())));
#endif
}

long long addMicrosClamped(long long lhs, long long rhs) {
  if (rhs > 0 && lhs > std::numeric_limits<long long>::max() - rhs) {
    return std::numeric_limits<long long>::max();
  }
  if (rhs < 0 && lhs < std::numeric_limits<long long>::min() - rhs) {
    return std::numeric_limits<long long>::min();
  }
  return lhs + rhs;
}

struct AudioClockAnchor {
  long long micros;
  long long wallMicros;
  long long endMicros;
  int ratePercent;
};

bool acquireAudioClockAnchorWriter(UserData *userData, bool mayWait) {
  // The render callback passes false: its publication path never waits and
  // performs only atomic operations.
  if (!mayWait) {
    return !userData->audioClockAnchorWriter->test_and_set(
        std::memory_order_acquire);
  }
  while (userData->audioClockAnchorWriter->test_and_set(
      std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  return true;
}

void publishAudioClockAnchor(UserData *userData, AudioClockAnchor anchor,
                             bool mayWait) {
  if (!acquireAudioClockAnchorWriter(userData, mayWait)) {
    return;
  }

  // Odd generations are in flight; even generations are complete. The final
  // release pairs with the reader's acquire before it accepts the tuple.
  userData->audioClockAnchorSequence->fetch_add(1, std::memory_order_acq_rel);
  userData->audioClockAnchorMicros->store(anchor.micros,
                                          std::memory_order_relaxed);
  userData->audioClockAnchorWallMicros->store(anchor.wallMicros,
                                              std::memory_order_relaxed);
  userData->audioClockAnchorEndMicros->store(anchor.endMicros,
                                             std::memory_order_relaxed);
  userData->audioClockAnchorRatePercent->store(anchor.ratePercent,
                                               std::memory_order_relaxed);
  userData->audioClockAnchorSequence->fetch_add(1, std::memory_order_release);
  userData->audioClockAnchorWriter->clear(std::memory_order_release);
}

AudioClockAnchor readAudioClockAnchor(const UserData &userData) {
  for (;;) {
    const std::uint64_t generationBefore =
        userData.audioClockAnchorSequence->load(std::memory_order_acquire);
    if ((generationBefore & 1U) != 0) {
      std::this_thread::yield();
      continue;
    }

    const AudioClockAnchor anchor{
        .micros =
            userData.audioClockAnchorMicros->load(std::memory_order_relaxed),
        .wallMicros = userData.audioClockAnchorWallMicros->load(
            std::memory_order_relaxed),
        .endMicros =
            userData.audioClockAnchorEndMicros->load(std::memory_order_relaxed),
        .ratePercent = userData.audioClockAnchorRatePercent->load(
            std::memory_order_relaxed),
    };
    const std::uint64_t generationAfter =
        userData.audioClockAnchorSequence->load(std::memory_order_acquire);
    if (generationBefore == generationAfter) {
      return anchor;
    }
  }
}

long long beginAudioClockBuffer(UserData *userData, ma_uint32 frameCount,
                                int sampleRate, int playbackRatePercent) {
  const int64_t startFrame = userData->audioClockFrameCursor->fetch_add(
      frameCount, std::memory_order_acq_rel);
  const long long baseMicros =
      userData->audioClockBaseMicros->load(std::memory_order_acquire);
  const long long bufferStartMicros =
      addMicrosClamped(baseMicros, framesToChartMicros(startFrame, sampleRate,
                                                       playbackRatePercent));
  const long long bufferEndMicros = addMicrosClamped(
      baseMicros, framesToChartMicros(startFrame + frameCount, sampleRate,
                                      playbackRatePercent));

  publishAudioClockAnchor(userData,
                          {.micros = bufferStartMicros,
                           .wallMicros = nowMicros(),
                           .endMicros = bufferEndMicros,
                           .ratePercent = playbackRatePercent},
                          false);
  return bufferStartMicros;
}

void fillSilence(void *pOutput, ma_uint32 frameCount, int outputChannels) {
  std::fill_n((ma_int16 *)pOutput, frameCount * outputChannels, 0);
}

// Mixing logic extracted to be backend-agnostic
void mixAudio(void *pOutput, ma_uint32 frameCount, int outputChannels,
              UserData *userData) {
  if (userData == nullptr || userData->callbackState == nullptr) {
    return;
  }

  AudioCallbackState &state = *userData->callbackState;
  audio::playback::DrainRealtimeCommands(state);
  audio::playback::DrainCommands(state);

  if (!userData->stopwatch->isRunning()) {
    fillSilence(pOutput, frameCount, outputChannels);
    return;
  }

  int sampleRate = userData->sampleRate ? userData->sampleRate->load() : 44100;
  if (sampleRate <= 0) {
    sampleRate = 44100;
  }
  const int playbackRatePercent =
      userData->playbackRatePercent
          ? userData->playbackRatePercent->load(std::memory_order_acquire)
          : 100;

  const long long bufferStartMicros = beginAudioClockBuffer(
      userData, frameCount, sampleRate, playbackRatePercent);
  audio::playback::ActivateScheduledSounds(state, bufferStartMicros, sampleRate,
                                           frameCount, playbackRatePercent);

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

  const float bgmGain = userData->bgmGain
                            ? userData->bgmGain->load(std::memory_order_acquire)
                            : 1.0f;
  const float keysoundGain =
      userData->keysoundGain
          ? userData->keysoundGain->load(std::memory_order_acquire)
          : 1.0f;
  audio::playback::MixActiveSounds(
      state, std::span<float>(mixBuffer, requiredSamples), frameCount,
      outputChannels, bgmGain, keysoundGain, playbackRatePercent);

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

} // namespace

class ConfigurableBackendLifecycle final
    : public audio::playback::IBackendLifecycle {
public:
  explicit ConfigurableBackendLifecycle(std::unique_ptr<audio::IBackend> backend)
      : backend_(std::move(backend)) {}

  audio::playback::BackendStateObservation observeState() const override {
    if (!backend_) {
      return {.state = audio::playback::BackendRunState::Unknown,
              .diagnostic = "Audio stream is unavailable"};
    }
    return backend_->observeState();
  }

  int outputSampleRate() const override {
    return backend_ == nullptr
               ? 0
               : static_cast<int>(backend_->runtimeState().effectiveSampleRate);
  }

  audio::playback::BackendOperationResult stopAndDrain() override {
    std::string error;
    return {.success = backend_ != nullptr && backend_->stop(error),
            .diagnostic = std::move(error)};
  }

  audio::playback::BackendOperationResult start() override {
    std::string error;
    return {.success = backend_ != nullptr && backend_->start(error),
            .diagnostic = std::move(error)};
  }

  [[nodiscard]] audio::RuntimeState runtimeState() const {
    return backend_ == nullptr ? audio::RuntimeState{}
                               : backend_->runtimeState();
  }

private:
  std::unique_ptr<audio::IBackend> backend_;
};

void configurableBackendRender(void *output, std::uint32_t frameCount,
                               int outputChannels, void *userData) {
  mixAudio(output, static_cast<ma_uint32>(frameCount), outputChannels,
           static_cast<UserData *>(userData));
}

// AudioWrapper Implementation

void AudioWrapper::initializeUserData() {
  userData.callbackState = &callbackState;
  userData.sampleRate = &currentSampleRate;
  userData.audioClockBaseMicros = &audioClockBaseMicros;
  userData.audioClockFrameCursor = &audioClockFrameCursor;
  userData.audioClockAnchorMicros = &audioClockAnchorMicros;
  userData.audioClockAnchorWallMicros = &audioClockAnchorWallMicros;
  userData.audioClockAnchorEndMicros = &audioClockAnchorEndMicros;
  userData.audioClockAnchorRatePercent = &audioClockAnchorRatePercent;
  userData.audioClockAnchorSequence = &audioClockAnchorSequence;
  userData.audioClockAnchorWriter = &audioClockAnchorWriter;
  userData.playbackRatePercent = &playbackRatePercent;
  userData.bgmGain = &bgmGain;
  userData.keysoundGain = &keysoundGain;
  userData.stopwatch = stopwatch;
  userData.mixBuffer = &mixBuffer;
  userData.bassFilter = &bassFilter;
  userData.trebleFilter = &trebleFilter;
  userData.reverb = &reverb;
  userData.compressor = &compressor;
}

void AudioWrapper::startBackendAfterConstruction() {
  const auto started = startDevice();
  if (!started.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Audio startup failed: %s",
                 started.diagnostic.c_str());
  }
}

AudioWrapper::AudioWrapper(Stopwatch *stopwatch)
    : AudioWrapper(stopwatch, audio::CreatePlatformBackendFactory()) {}

AudioWrapper::AudioWrapper(
    Stopwatch *stopwatch,
    std::unique_ptr<audio::IBackendFactory> injectedFactory)
    : backendFactory(std::move(injectedFactory)), stopwatch(stopwatch) {
  initializeUserData();

  std::string openError;
  auto opened = backendFactory != nullptr
                    ? backendFactory->open({}, configurableBackendRender,
                                           &userData, openError)
                    : nullptr;
  if (!opened) {
    throw std::runtime_error(openError.empty()
                                 ? "Failed to initialize audio backend"
                                 : std::move(openError));
  }
  runtimeState_ = opened->runtimeState();
  backend =
      std::make_unique<ConfigurableBackendLifecycle>(std::move(opened));

  startBackendAfterConstruction();

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

AudioWrapper::AudioWrapper(
    Stopwatch *stopwatch,
    std::unique_ptr<audio::playback::IBackendLifecycle> injectedBackend)
    : backend(std::move(injectedBackend)), stopwatch(stopwatch) {
  initializeUserData();
  startBackendAfterConstruction();
  runtimeState_.request = {};
  runtimeState_.effectiveSampleRate = static_cast<std::uint32_t>(
      std::max(0, backend != nullptr ? backend->outputSampleRate() : 0));
}

AudioWrapper::~AudioWrapper() {
  const auto unloaded = unloadSounds();
  if (!unloaded.success) {
    SDL_LogCritical(SDL_LOG_CATEGORY_AUDIO,
                    "Audio shutdown could not confirm callback drain: %s",
                    unloaded.diagnostic.c_str());
    std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
    closeRealtimeSoundGateAndWait();
    std::lock_guard<std::mutex> soundDataLock(soundDataListMutex);
    std::lock_guard<std::mutex> commandLock(audioCommandMutex);
    backend.reset();
    backendState.store(audio::playback::BackendRunState::Stopped,
                       std::memory_order_release);
    clearCallbackState();
    soundDataList.clear();
    soundDataIndexMap.clear();
    skinSounds.clear();
    retiredSkinSounds.clear();
    skinSoundDecodedBytes = 0;
  }
}

long long AudioWrapper::getTimeMicros() const {
  const AudioClockAnchor anchor = readAudioClockAnchor(userData);

  if (anchor.wallMicros <= 0 || !stopwatch->isRunning()) {
    audioClockPublishedMicros.store(anchor.micros, std::memory_order_release);
    return anchor.micros;
  }

  const audio::PlaybackRate rate{.percent = anchor.ratePercent};
  const long long wallDeltaMicros = nowMicros() - anchor.wallMicros;
  long long interpolatedMicros = addMicrosClamped(
      anchor.micros, rate.chartMicrosFromReal(wallDeltaMicros));
  if (anchor.endMicros >= anchor.micros &&
      interpolatedMicros > anchor.endMicros) {
    interpolatedMicros = anchor.endMicros;
  }
  if (interpolatedMicros < anchor.micros) {
    interpolatedMicros = anchor.micros;
  }
  audioClockPublishedMicros.store(interpolatedMicros,
                                  std::memory_order_release);
  return interpolatedMicros;
}

std::optional<long long> AudioWrapper::songTimeMicrosAtSteadyMicros(
    long long steadyMicros) const noexcept {
  const AudioClockAnchor anchor = readAudioClockAnchor(userData);
  if (anchor.wallMicros <= 0 || anchor.ratePercent <= 0) {
    return std::nullopt;
  }
  const audio::PlaybackRate rate{.percent = anchor.ratePercent};
  if (!rate.valid()) {
    return std::nullopt;
  }
  const long long interpolatedMicros = addMicrosClamped(
      anchor.micros,
      rate.chartMicrosFromReal(steadyMicros - anchor.wallMicros));
  if (anchor.endMicros >= anchor.micros &&
      interpolatedMicros > anchor.endMicros) {
    return anchor.endMicros;
  }
  return interpolatedMicros;
}

void AudioWrapper::seekClock(long long micros) {
  std::lock_guard<std::mutex> lock(audioCommandMutex);
  const long long wallMicros = nowMicros();
  audioClockBaseMicros.store(micros, std::memory_order_release);
  audioClockFrameCursor.store(0, std::memory_order_release);
  publishAudioClockAnchor(
      &userData,
      {.micros = micros,
       .wallMicros = wallMicros,
       .endMicros = micros,
       .ratePercent = playbackRatePercent.load(std::memory_order_acquire)},
      true);
  audioClockPublishedMicros.store(micros, std::memory_order_release);
}

bool AudioWrapper::setPlaybackRate(audio::PlaybackRate rate,
                                   std::string &errorMessage) {
  errorMessage.clear();
  if (rate.mode == audio::PlaybackMode::TimeStretch) {
    errorMessage = "TimeStretch playback mode is not supported";
    return false;
  }
  if (!rate.valid()) {
    errorMessage =
        "Playback rate must be 50%-200% in five-percent PitchShift steps";
    return false;
  }

  std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
  if (backend == nullptr) {
    backendState.store(audio::playback::BackendRunState::Unknown,
                       std::memory_order_release);
    errorMessage = "Audio backend is unavailable for playback-rate mutation";
    return false;
  }
  const auto observed = backend->observeState();
  backendState.store(observed.state, std::memory_order_release);
  if (!audio::playback::CanMutateCallbackStateDirectly(observed.state)) {
    errorMessage = "Audio playback must be stopped before changing playback "
                   "rate";
    if (!observed.diagnostic.empty()) {
      errorMessage += ": " + observed.diagnostic;
    }
    return false;
  }
  closeRealtimeSoundGateAndWait();

  std::lock_guard<std::mutex> commandLock(audioCommandMutex);
  const long long rebasedMicros =
      stopwatch->isRunning()
          ? getTimeMicros()
          : audioClockPublishedMicros.load(std::memory_order_acquire);
  const long long wallMicros = nowMicros();
  audioClockBaseMicros.store(rebasedMicros, std::memory_order_release);
  audioClockFrameCursor.store(0, std::memory_order_release);
  playbackRatePercent.store(rate.percent, std::memory_order_release);
  publishAudioClockAnchor(&userData,
                          {.micros = rebasedMicros,
                           .wallMicros = wallMicros,
                           .endMicros = rebasedMicros,
                           .ratePercent = rate.percent},
                          true);
  audioClockPublishedMicros.store(rebasedMicros, std::memory_order_release);
  return true;
}

audio::PlaybackRate AudioWrapper::playbackRate() const {
  return {.percent = playbackRatePercent.load(std::memory_order_acquire),
          .mode = audio::PlaybackMode::PitchShift};
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

bool AudioWrapper::loadSoundFromMemory(const path_t &path,
                                       const std::vector<unsigned char> &bytes,
                                       std::atomic<bool> &isCancelled) {
  {
    std::lock_guard<std::mutex> lock(soundDataListMutex);
    if (soundDataIndexMap.contains(path)) {
      return true;
    }
  }

  std::vector<short> pcmData;
  SF_INFO sfInfo;
  bool result =
      decodeAudioBytesToPCM(path, bytes, pcmData, sfInfo, isCancelled);
  if (!result) {
    SDL_Log("Failed to decode audio file %s", path_t_to_utf8(path).c_str());
    return false;
  }
  return loadDecodedSound(path, std::move(pcmData), sfInfo.channels,
                          sfInfo.samplerate, isCancelled);
}

bool AudioWrapper::loadGeneratedSound(const path_t &path,
                                      std::vector<short> pcmData, int channels,
                                      int sampleRate) {
  {
    std::lock_guard<std::mutex> lock(soundDataListMutex);
    if (soundDataIndexMap.contains(path)) {
      return true;
    }
  }

  std::atomic_bool isCancelled = false;
  return loadDecodedSound(path, std::move(pcmData), channels, sampleRate,
                          isCancelled);
}

bool AudioWrapper::loadDecodedSound(const path_t &path,
                                    std::vector<short> pcmData, int channels,
                                    int sampleRate,
                                    std::atomic<bool> &isCancelled) {
  if (isCancelled) {
    return false;
  }
  if (channels <= 0 || sampleRate <= 0 ||
      pcmData.size() % static_cast<size_t>(channels) != 0) {
    SDL_Log("Invalid decoded PCM format for %s", path_t_to_utf8(path).c_str());
    return false;
  }

  auto soundData = std::make_shared<SoundData>();

  soundData->currentFrame = 0;
  soundData->channels = channels;
  soundData->sourceSampleRate = sampleRate;
  soundData->playing = false;
  soundData->sourceData = std::move(pcmData);
  soundData->sourceFrameCount =
      soundData->sourceData.size() / static_cast<size_t>(channels);

  std::lock_guard<std::mutex> lock(soundDataListMutex);
  if (soundDataIndexMap.contains(path)) {
    return true;
  }
  const int targetSampleRate =
      currentSampleRate.load(std::memory_order_acquire);
  SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION,
                 "Target sample rate: %d, File sample rate: %d",
                 targetSampleRate, sampleRate);

  soundData->outputData = audio::ResamplePcm(soundData->sourceData, channels,
                                             sampleRate, targetSampleRate);
  if ((!soundData->sourceData.empty() && soundData->outputData.empty()) ||
      isCancelled) {
    return false;
  }
  soundData->outputFrameCount =
      soundData->outputData.size() / static_cast<size_t>(channels);
  soundDataIndexMap[path] = soundDataList.size();
  soundDataList.push_back(soundData);
  return true;
}

void AudioWrapper::preloadSounds(const std::vector<path_t> &paths,
                                 std::atomic<bool> &isCancelled) {
  for (const auto &path : paths) {
    loadSound(path, isCancelled);
  }
}

void AudioWrapper::clearCallbackState() {
  audio::playback::ClearCallbackSounds(callbackState);
  callbackState.commandReadCursor.store(0, std::memory_order_release);
  callbackState.commandWriteCursor.store(0, std::memory_order_release);
  callbackState.realtimeCommandReadCursor.store(0, std::memory_order_release);
  callbackState.realtimeCommandWriteCursor.store(0,
                                                 std::memory_order_release);
}

bool AudioWrapper::playSound(const path_t &path, audio::Bus bus,
                             long long startOffsetMicros) {
  std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
  std::lock_guard<std::mutex> soundDataLock(soundDataListMutex);

  const auto indexIt = soundDataIndexMap.find(path);
  if (indexIt == soundDataIndexMap.end()) {
    SDL_Log("Sound not found: %s", path_t_to_utf8(path).c_str());
    return false;
  }

  auto &soundData = soundDataList[indexIt->second];
  const auto started = startDeviceWithLifecycleAndSoundLocked();
  if (!started.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Audio start failed for %s: %s",
                 path_t_to_utf8(path).c_str(), started.diagnostic.c_str());
    return false;
  }
  const long long clampedOffsetMicros = std::max(0LL, startOffsetMicros);
  const size_t startFrame = static_cast<size_t>(
      std::min<long long>(static_cast<long long>(soundData->outputFrameCount),
                          clampedOffsetMicros *
                              static_cast<long long>(currentSampleRate.load(
                                  std::memory_order_acquire)) /
                              1000000LL));
  if (startFrame >= soundData->outputFrameCount) {
    return false;
  }

  {
    std::lock_guard<std::mutex> commandLock(audioCommandMutex);
    if (!audio::playback::EnqueueCommand(callbackState,
                                         {.type = AudioCommandType::PlayNow,
                                          .soundData = soundData.get(),
                                          .bus = bus,
                                          .startFrame = startFrame})) {
      SDL_Log("Audio command queue full; dropping %s",
              path_t_to_utf8(path).c_str());
      return false;
    }
  }
  return true;
}

void AudioWrapper::cleanupRetiredSkinSoundsLocked() noexcept {
  for (auto found = retiredSkinSounds.begin();
       found != retiredSkinSounds.end();) {
    if (found->acknowledgement != nullptr &&
        found->acknowledgement->load(std::memory_order_acquire)) {
      skinSoundDecodedBytes -= found->decodedBytes;
      found = retiredSkinSounds.erase(found);
    } else {
      ++found;
    }
  }
}

audio::SkinSoundLoadResult AudioWrapper::loadSkinSound(
    const path_t &path, std::atomic<bool> &isCancelled,
    std::size_t maximumEncodedBytes,
    std::size_t maximumTotalDecodedBytes, std::stop_token stop) noexcept {
  try {
    if (isCancelled || maximumTotalDecodedBytes == 0) {
      return {};
    }

    std::shared_ptr<SoundData> privateSound;
    {
      std::lock_guard<std::mutex> lock(soundDataListMutex);
      cleanupRetiredSkinSoundsLocked();
      if (const auto shared = soundDataIndexMap.find(path);
          shared != soundDataIndexMap.end()) {
        const auto &source = soundDataList[shared->second];
        if (source == nullptr ||
            source->sourceData.size() >
                std::numeric_limits<std::size_t>::max() -
                    source->outputData.size()) {
          return {};
        }
        const std::size_t samples =
            source->sourceData.size() + source->outputData.size();
        if (samples > std::numeric_limits<std::size_t>::max() / sizeof(short)) {
          return {};
        }
        const std::size_t decodedBytes = samples * sizeof(short);
        if (decodedBytes > maximumTotalDecodedBytes ||
            skinSoundDecodedBytes >
                maximumTotalDecodedBytes - decodedBytes) {
          return {};
        }
        privateSound = std::make_shared<SoundData>();
        privateSound->channels = source->channels;
        privateSound->sourceSampleRate = source->sourceSampleRate;
        privateSound->sourceData = source->sourceData;
        privateSound->outputData = source->outputData;
        privateSound->sourceFrameCount = source->sourceFrameCount;
        privateSound->outputFrameCount = source->outputFrameCount;
      }
    }

    if (privateSound == nullptr) {
      std::vector<short> pcmData;
      SF_INFO sfInfo{};
      const std::size_t maximumSamples =
          maximumTotalDecodedBytes / sizeof(short);
      if (!decodeAudioToPCMBounded(
              path, pcmData, sfInfo, isCancelled,
              {.maximumEncodedBytes = maximumEncodedBytes,
               .maximumPcmSamples = maximumSamples},
              stop) ||
          isCancelled || sfInfo.channels <= 0 || sfInfo.samplerate <= 0 ||
          pcmData.size() % static_cast<std::size_t>(sfInfo.channels) != 0) {
        return {};
      }
      privateSound = std::make_shared<SoundData>();
      privateSound->channels = sfInfo.channels;
      privateSound->sourceSampleRate = sfInfo.samplerate;
      privateSound->sourceData = std::move(pcmData);
      privateSound->sourceFrameCount =
          privateSound->sourceData.size() /
          static_cast<std::size_t>(privateSound->channels);
      privateSound->outputData = audio::ResamplePcm(
          privateSound->sourceData, privateSound->channels,
          privateSound->sourceSampleRate,
          currentSampleRate.load(std::memory_order_acquire));
      if ((!privateSound->sourceData.empty() &&
           privateSound->outputData.empty()) ||
          isCancelled) {
        return {};
      }
      privateSound->outputFrameCount =
          privateSound->outputData.size() /
          static_cast<std::size_t>(privateSound->channels);
    }

    if (privateSound->sourceData.size() >
        std::numeric_limits<std::size_t>::max() -
            privateSound->outputData.size()) {
      return {};
    }
    const std::size_t sampleCount = privateSound->sourceData.size() +
                                    privateSound->outputData.size();
    if (sampleCount >
        std::numeric_limits<std::size_t>::max() / sizeof(short)) {
      return {};
    }
    const std::size_t decodedBytes = sampleCount * sizeof(short);

    std::lock_guard<std::mutex> lock(soundDataListMutex);
    cleanupRetiredSkinSoundsLocked();
    if (isCancelled || decodedBytes > maximumTotalDecodedBytes ||
        skinSoundDecodedBytes > maximumTotalDecodedBytes - decodedBytes) {
      return {};
    }
    audio::SkinSoundHandle handle{.value = ++nextSkinSoundHandle};
    if (!handle) {
      handle.value = ++nextSkinSoundHandle;
    }
    auto [unused, inserted] = skinSounds.emplace(
        handle.value,
        SkinSoundRecord{.soundData = std::move(privateSound),
                        .decodedBytes = decodedBytes});
    (void)unused;
    if (!inserted) {
      return {};
    }
    skinSoundDecodedBytes += decodedBytes;
    return {.handle = handle, .decodedBytes = decodedBytes};
  } catch (...) {
    return {};
  }
}

bool AudioWrapper::playSkinSound(audio::SkinSoundHandle handle, float gain,
                                 bool loop) noexcept {
  try {
    std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
    std::lock_guard<std::mutex> soundDataLock(soundDataListMutex);
    cleanupRetiredSkinSoundsLocked();
    const auto found = skinSounds.find(handle.value);
    if (found == skinSounds.end() || found->second.soundData == nullptr) {
      return false;
    }
    const auto started = startDeviceWithLifecycleAndSoundLocked();
    if (!started.success) {
      return false;
    }
    std::lock_guard<std::mutex> commandLock(audioCommandMutex);
    return audio::playback::EnqueueCommand(
        callbackState,
        {.type = AudioCommandType::PlayNow,
         .soundData = found->second.soundData.get(),
         .bus = audio::Bus::System,
         .gain = gain,
         .loop = loop});
  } catch (...) {
    return false;
  }
}

bool AudioWrapper::stopSkinSound(audio::SkinSoundHandle handle) noexcept {
  try {
    std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
    std::lock_guard<std::mutex> soundDataLock(soundDataListMutex);
    cleanupRetiredSkinSoundsLocked();
    const auto found = skinSounds.find(handle.value);
    if (found == skinSounds.end() || found->second.soundData == nullptr) {
      return true;
    }
    const auto observed =
        backend != nullptr
            ? backend->observeState()
            : audio::playback::BackendStateObservation{
                  .state = audio::playback::BackendRunState::Stopped};
    backendState.store(observed.state, std::memory_order_release);
    std::lock_guard<std::mutex> commandLock(audioCommandMutex);
    if (audio::playback::CanMutateCallbackStateDirectly(observed.state)) {
      audio::playback::DrainCommands(callbackState);
      audio::playback::RemoveSound(callbackState,
                                   found->second.soundData.get());
      return true;
    }
    return audio::playback::EnqueueCommand(
        callbackState, {.type = AudioCommandType::StopOwner,
                        .soundData = found->second.soundData.get()});
  } catch (...) {
    return false;
  }
}

bool AudioWrapper::disposeSkinSound(audio::SkinSoundHandle handle) noexcept {
  try {
    std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
    std::lock_guard<std::mutex> soundDataLock(soundDataListMutex);
    cleanupRetiredSkinSoundsLocked();
    const auto found = skinSounds.find(handle.value);
    if (found == skinSounds.end() || found->second.soundData == nullptr) {
      return true;
    }
    const auto observed =
        backend != nullptr
            ? backend->observeState()
            : audio::playback::BackendStateObservation{
                  .state = audio::playback::BackendRunState::Stopped};
    backendState.store(observed.state, std::memory_order_release);
    if (audio::playback::CanMutateCallbackStateDirectly(observed.state)) {
      std::lock_guard<std::mutex> commandLock(audioCommandMutex);
      audio::playback::DrainCommands(callbackState);
      audio::playback::RemoveSound(callbackState,
                                   found->second.soundData.get());
      skinSoundDecodedBytes -= found->second.decodedBytes;
      skinSounds.erase(found);
      return true;
    }
    found->second.acknowledgement = std::make_shared<std::atomic_bool>(false);
    {
      std::lock_guard<std::mutex> commandLock(audioCommandMutex);
      if (!audio::playback::EnqueueCommand(
              callbackState,
              {.type = AudioCommandType::StopOwner,
               .soundData = found->second.soundData.get(),
               .acknowledgement = found->second.acknowledgement.get()})) {
        found->second.acknowledgement.reset();
        return false;
      }
    }
    retiredSkinSounds.push_back(std::move(found->second));
    skinSounds.erase(found);
    return true;
  } catch (...) {
    return false;
  }
}

bool AudioWrapper::playSkinSound(const path_t &path, float gain, bool loop) {
  std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
  std::lock_guard<std::mutex> soundDataLock(soundDataListMutex);

  const auto indexIt = soundDataIndexMap.find(path);
  if (indexIt == soundDataIndexMap.end()) {
    SDL_Log("Skin sound not found: %s", path_t_to_utf8(path).c_str());
    return false;
  }
  const auto started = startDeviceWithLifecycleAndSoundLocked();
  if (!started.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Skin audio start failed for %s: %s",
                 path_t_to_utf8(path).c_str(), started.diagnostic.c_str());
    return false;
  }
  std::lock_guard<std::mutex> commandLock(audioCommandMutex);
  return audio::playback::EnqueueCommand(
      callbackState,
      {.type = AudioCommandType::PlayNow,
       .soundData = soundDataList[indexIt->second].get(),
       .bus = audio::Bus::System,
       .gain = gain,
       .loop = loop});
}

audio::playback::BackendOperationResult
AudioWrapper::stopSkinSound(const path_t &path) {
  return mutateSkinSound(path, false);
}

audio::playback::BackendOperationResult
AudioWrapper::disposeSkinSound(const path_t &path) {
  return mutateSkinSound(path, true);
}

audio::playback::BackendOperationResult
AudioWrapper::mutateSkinSound(const path_t &path, bool dispose) {
  std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
  std::lock_guard<std::mutex> soundDataLock(soundDataListMutex);
  const auto indexIt = soundDataIndexMap.find(path);
  if (indexIt == soundDataIndexMap.end()) {
    return {.success = true};
  }
  if (!backend) {
    closeRealtimeSoundGateAndWait();
    backendState.store(audio::playback::BackendRunState::Unknown,
                       std::memory_order_release);
    return {.success = false, .diagnostic = "Audio backend is unavailable"};
  }

  closeRealtimeSoundGateAndWait();
  const auto observed = backend->observeState();
  backendState.store(observed.state, std::memory_order_release);
  if (observed.state == audio::playback::BackendRunState::Unknown) {
    return {.success = false,
            .diagnostic = observed.diagnostic.empty()
                              ? "Audio backend state is unavailable"
                              : observed.diagnostic};
  }
  const bool restart =
      observed.state == audio::playback::BackendRunState::Running;
  if (restart) {
    const auto stopped = backend->stopAndDrain();
    if (!stopped.success) {
      openRealtimeSoundGate();
      return stopped;
    }
    backendState.store(audio::playback::BackendRunState::Stopped,
                       std::memory_order_release);
  }

  SoundData *target = soundDataList[indexIt->second].get();
  {
    std::lock_guard<std::mutex> commandLock(audioCommandMutex);
    audio::playback::DrainRealtimeCommands(callbackState);
    audio::playback::DrainCommands(callbackState);
    audio::playback::RemoveSound(callbackState, target);
  }

  if (dispose) {
    const std::size_t removedIndex = indexIt->second;
    soundDataList.erase(soundDataList.begin() + removedIndex);
    soundDataIndexMap.erase(indexIt);
    for (auto &[retainedPath, retainedIndex] : soundDataIndexMap) {
      (void)retainedPath;
      if (retainedIndex > removedIndex) {
        --retainedIndex;
      }
    }
  }

  if (!restart) {
    return {.success = true};
  }
  const auto started = startDeviceWithLifecycleAndSoundLocked();
  if (!started.success) {
    return started;
  }
  return {.success = true};
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
      static_cast<double>(soundData->outputFrameCount) * 1000000.0 /
      static_cast<double>(sampleRate));
}

bool AudioWrapper::scheduleSound(const path_t &path, audio::Bus bus,
                                 long long startMicros) {
  std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
  std::lock_guard<std::mutex> soundDataLock(soundDataListMutex);

  const auto indexIt = soundDataIndexMap.find(path);
  if (indexIt == soundDataIndexMap.end()) {
    SDL_Log("Sound not found: %s", path_t_to_utf8(path).c_str());
    return false;
  }

  auto &soundData = soundDataList[indexIt->second];
  const auto started = startDeviceWithLifecycleAndSoundLocked();
  if (!started.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                 "Audio start failed for scheduled %s: %s",
                 path_t_to_utf8(path).c_str(), started.diagnostic.c_str());
    return false;
  }
  const uint64_t sequence =
      scheduledSoundSequence.fetch_add(1, std::memory_order_acq_rel);

  {
    std::lock_guard<std::mutex> commandLock(audioCommandMutex);
    if (!audio::playback::EnqueueCommand(callbackState,
                                         {.type = AudioCommandType::Schedule,
                                          .soundData = soundData.get(),
                                          .bus = bus,
                                          .startMicros = startMicros,
                                          .sequence = sequence})) {
      SDL_Log("Audio command queue full; dropping scheduled %s",
              path_t_to_utf8(path).c_str());
      return false;
    }
  }

  return true;
}

std::optional<audio::RealtimeSoundHandle>
AudioWrapper::resolveRealtimeSound(const path_t &path) const {
  std::lock_guard<std::mutex> lock(soundDataListMutex);
  const auto indexIt = soundDataIndexMap.find(path);
  if (indexIt == soundDataIndexMap.end() ||
      indexIt->second >= soundDataList.size() ||
      soundDataList[indexIt->second] == nullptr) {
    return std::nullopt;
  }
  return audio::RealtimeSoundHandle(soundDataList[indexIt->second]);
}

std::optional<RealtimeAudioCommandReservation>
AudioWrapper::tryReserveRealtimeSoundCommand() const noexcept {
  if (!realtimeSoundGateOpen.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  realtimeSoundReservations.fetch_add(1, std::memory_order_acq_rel);
  if (!realtimeSoundGateOpen.load(std::memory_order_acquire) ||
      backendState.load(std::memory_order_acquire) !=
          audio::playback::BackendRunState::Running) {
    releaseRealtimeSoundReservation();
    return std::nullopt;
  }
  auto reservation =
      audio::playback::TryReserveRealtimeCommand(callbackState);
  if (!reservation.has_value()) {
    releaseRealtimeSoundReservation();
  }
  return reservation;
}

bool AudioWrapper::commitRealtimeKeysound(
    RealtimeAudioCommandReservation reservation,
    const audio::RealtimeSoundHandle &handle, size_t startFrame) noexcept {
  const bool committed =
      backendState.load(std::memory_order_acquire) ==
          audio::playback::BackendRunState::Running &&
      handle.valid() &&
      audio::playback::CommitRealtimeCommand(
          callbackState, reservation,
          {.type = AudioCommandType::PlayNow,
           .soundData = handle.soundData_.get(),
           .bus = audio::Bus::Keysound,
           .startFrame = startFrame});
  releaseRealtimeSoundReservation();
  return committed;
}

void AudioWrapper::cancelRealtimeSoundCommand(
    RealtimeAudioCommandReservation reservation) noexcept {
  (void)reservation;
  releaseRealtimeSoundReservation();
}

void AudioWrapper::openRealtimeSoundGate() noexcept {
  realtimeSoundGateOpen.store(true, std::memory_order_release);
}

void AudioWrapper::closeRealtimeSoundGateAndWait() noexcept {
  realtimeSoundGateOpen.store(false, std::memory_order_release);
  std::uint32_t reservations =
      realtimeSoundReservations.load(std::memory_order_acquire);
  while (reservations != 0) {
    realtimeSoundReservations.wait(reservations, std::memory_order_acquire);
    reservations =
        realtimeSoundReservations.load(std::memory_order_acquire);
  }
}

void AudioWrapper::releaseRealtimeSoundReservation() const noexcept {
  if (realtimeSoundReservations.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    realtimeSoundReservations.notify_all();
  }
}

bool AudioWrapper::stageScheduledSound(const path_t &path, audio::Bus bus,
                                       long long startMicros) {
  std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
  std::lock_guard<std::mutex> soundDataLock(soundDataListMutex);

  const auto indexIt = soundDataIndexMap.find(path);
  if (indexIt == soundDataIndexMap.end()) {
    SDL_Log("Sound not found: %s", path_t_to_utf8(path).c_str());
    return false;
  }

  if (!backend) {
    closeRealtimeSoundGateAndWait();
    backendState.store(audio::playback::BackendRunState::Unknown,
                       std::memory_order_release);
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                 "Audio backend is unavailable for scheduled %s",
                 path_t_to_utf8(path).c_str());
    return false;
  }
  const auto observed = backend->observeState();
  backendState.store(observed.state, std::memory_order_release);
  if (!audio::playback::CanMutateCallbackStateDirectly(observed.state)) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO,
                 "Audio backend is not stopped for staged %s: %s",
                 path_t_to_utf8(path).c_str(), observed.diagnostic.c_str());
    return false;
  }
  closeRealtimeSoundGateAndWait();

  auto &soundData = soundDataList[indexIt->second];
  const uint64_t sequence =
      scheduledSoundSequence.fetch_add(1, std::memory_order_acq_rel);

  {
    std::lock_guard<std::mutex> commandLock(audioCommandMutex);
    if (!audio::playback::InsertScheduledSound(
            callbackState, {.soundData = soundData.get(),
                            .bus = bus,
                            .startMicros = startMicros,
                            .sequence = sequence})) {
      SDL_Log("Audio scheduling capacity exhausted; dropping scheduled %s",
              path_t_to_utf8(path).c_str());
      return false;
    }
  }

  return true;
}

audio::playback::BackendOperationResult AudioWrapper::startDevice() {
  std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
  std::lock_guard<std::mutex> soundDataLock(soundDataListMutex);
  return startDeviceWithLifecycleAndSoundLocked();
}

audio::playback::BackendOperationResult
AudioWrapper::startDeviceWithLifecycleAndSoundLocked() {
  if (!backend) {
    closeRealtimeSoundGateAndWait();
    backendState.store(audio::playback::BackendRunState::Unknown,
                       std::memory_order_release);
    return {.success = false, .diagnostic = "Audio backend is unavailable"};
  }

  int targetSampleRate = backend->outputSampleRate();
  if (targetSampleRate <= 0) {
    targetSampleRate = 44100;
  }
  const auto observed = backend->observeState();
  backendState.store(observed.state, std::memory_order_release);
  if (observed.state == audio::playback::BackendRunState::Running &&
      currentSampleRate.load(std::memory_order_acquire) == targetSampleRate) {
    openRealtimeSoundGate();
    return {.success = true};
  }

  closeRealtimeSoundGateAndWait();
  std::lock_guard<std::mutex> commandLock(audioCommandMutex);
  std::vector<SoundData *> sounds;
  sounds.reserve(soundDataList.size());
  for (const auto &soundData : soundDataList) {
    if (soundData != nullptr) {
      sounds.push_back(soundData.get());
    }
  }
  for (const auto &[identity, record] : skinSounds) {
    (void)identity;
    if (record.soundData != nullptr) {
      sounds.push_back(record.soundData.get());
    }
  }
  for (const auto &record : retiredSkinSounds) {
    if (record.soundData != nullptr) {
      sounds.push_back(record.soundData.get());
    }
  }

  auto result = audio::playback::EnsureBackendStartedAtOutputRate(
      *backend, sounds, callbackState, targetSampleRate, currentSampleRate,
      audioClockFrameCursor, backendState);
  if (result.success) {
    openRealtimeSoundGate();
    if (const auto *configurable =
            dynamic_cast<const ConfigurableBackendLifecycle *>(backend.get())) {
      runtimeState_ = configurable->runtimeState();
    } else {
      runtimeState_.effectiveSampleRate =
          static_cast<std::uint32_t>(std::max(0, targetSampleRate));
    }
  }
  return result;
}

audio::playback::BackendOperationResult AudioWrapper::stopSounds() {
  std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
  std::lock_guard<std::mutex> commandLock(audioCommandMutex);
  return stopSoundsWithLifecycleAndCommandLocked();
}

audio::playback::BackendOperationResult
AudioWrapper::stopSoundsWithLifecycleAndCommandLocked() {
  closeRealtimeSoundGateAndWait();
  if (!backend) {
    backendState.store(audio::playback::BackendRunState::Stopped,
                       std::memory_order_release);
    clearCallbackState();
    return {.success = true};
  }
  const auto stopped = audio::playback::StopBackendAndClearCallbackState(
      *backend, callbackState, backendState);
  if (!stopped.success) {
    SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "Audio stop failed: %s",
                 stopped.diagnostic.c_str());
  }
  return stopped;
}

audio::playback::BackendOperationResult
AudioWrapper::unloadSound(const path_t &path) {
  return pruneSounds({path});
}

audio::playback::BackendOperationResult
AudioWrapper::pruneSounds(const std::vector<path_t> &paths) {
  std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
  std::lock_guard<std::mutex> soundDataLock(soundDataListMutex);
  std::vector<size_t> removedIndices;
  removedIndices.reserve(paths.size());
  for (const path_t &path : paths) {
    if (const auto indexIt = soundDataIndexMap.find(path);
        indexIt != soundDataIndexMap.end()) {
      removedIndices.push_back(indexIt->second);
    }
  }
  std::sort(removedIndices.begin(), removedIndices.end());
  removedIndices.erase(
      std::unique(removedIndices.begin(), removedIndices.end()),
      removedIndices.end());
  if (removedIndices.empty()) {
    return {.success = true};
  }

  std::lock_guard<std::mutex> commandLock(audioCommandMutex);
  const auto stopped = stopSoundsWithLifecycleAndCommandLocked();
  if (!stopped.success) {
    return stopped;
  }

  for (auto indexIt = removedIndices.rbegin(); indexIt != removedIndices.rend();
       ++indexIt) {
    soundDataList.erase(soundDataList.begin() + *indexIt);
  }
  for (auto mapIt = soundDataIndexMap.begin();
       mapIt != soundDataIndexMap.end();) {
    const size_t oldIndex = mapIt->second;
    if (std::binary_search(removedIndices.begin(), removedIndices.end(),
                           oldIndex)) {
      mapIt = soundDataIndexMap.erase(mapIt);
      continue;
    }
    mapIt->second -= static_cast<size_t>(
        std::distance(removedIndices.begin(),
                      std::lower_bound(removedIndices.begin(),
                                       removedIndices.end(), oldIndex)));
    ++mapIt;
  }
  return {.success = true};
}

audio::playback::BackendOperationResult AudioWrapper::unloadSounds() {
  std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
  std::lock_guard<std::mutex> soundDataLock(soundDataListMutex);
  std::lock_guard<std::mutex> commandLock(audioCommandMutex);
  const auto stopped = stopSoundsWithLifecycleAndCommandLocked();
  if (!stopped.success) {
    return stopped;
  }
  soundDataList.clear();
  soundDataIndexMap.clear();
  skinSounds.clear();
  retiredSkinSounds.clear();
  skinSoundDecodedBytes = 0;
  return {.success = true};
}

audio::Capabilities AudioWrapper::capabilities() const {
  std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
  return backendFactory != nullptr ? backendFactory->capabilities()
                                   : audio::Capabilities{};
}

audio::RuntimeState AudioWrapper::runtimeState() const {
  std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
  return runtimeState_;
}

bool AudioWrapper::restart(const audio::StreamRequest &request,
                           std::string &errorMessage) {
  errorMessage.clear();
  std::lock_guard<std::mutex> lifecycleLock(deviceLifecycleMutex);
  if (backendFactory == nullptr) {
    errorMessage = "Injected audio backend does not support reconfiguration";
    return false;
  }
  if (backend != nullptr) {
    if (backend->observeState().state !=
        audio::playback::BackendRunState::Stopped) {
      errorMessage = "Audio playback must be suspended before reconfiguration";
      return false;
    }
    closeRealtimeSoundGateAndWait();
    backend.reset();
    backendState.store(audio::playback::BackendRunState::Stopped,
                       std::memory_order_release);
  }

  auto candidate = backendFactory->open(request, configurableBackendRender,
                                        &userData, errorMessage);
  if (!candidate) {
    if (errorMessage.empty()) {
      errorMessage = "Audio backend could not open the requested stream";
    }
    return false;
  }
  const audio::RuntimeState candidateState = candidate->runtimeState();
  const int targetSampleRate =
      candidateState.effectiveSampleRate == 0
          ? 44100
          : static_cast<int>(candidateState.effectiveSampleRate);

  std::lock_guard<std::mutex> soundDataLock(soundDataListMutex);
  std::lock_guard<std::mutex> commandLock(audioCommandMutex);
  std::vector<SoundData *> sounds;
  sounds.reserve(soundDataList.size());
  for (const auto &soundData : soundDataList) {
    if (soundData != nullptr) {
      sounds.push_back(soundData.get());
    }
  }
  for (const auto &[identity, record] : skinSounds) {
    (void)identity;
    if (record.soundData != nullptr) {
      sounds.push_back(record.soundData.get());
    }
  }
  for (const auto &record : retiredSkinSounds) {
    if (record.soundData != nullptr) {
      sounds.push_back(record.soundData.get());
    }
  }
  const int previousSampleRate =
      currentSampleRate.load(std::memory_order_acquire);
  std::optional<audio::playback::OutputRateTransition> transition;
  if (previousSampleRate != targetSampleRate) {
    transition = audio::playback::PrepareOutputRateTransition(
        sounds, previousSampleRate, targetSampleRate);
    if (!transition.has_value()) {
      errorMessage = "Unable to prepare PCM for requested output sample rate";
      return false;
    }
  }

  if (!candidate->start(errorMessage)) {
    if (errorMessage.empty()) {
      errorMessage = "Audio backend could not start the requested stream";
    }
    backendState.store(audio::playback::BackendRunState::Stopped,
                       std::memory_order_release);
    return false;
  }

  if (transition.has_value()) {
    audio::playback::CommitOutputRateTransition(std::move(*transition),
                                                callbackState);
    const auto previousClockFrame =
        audioClockFrameCursor.load(std::memory_order_acquire);
    if (previousClockFrame > 0) {
      const std::size_t remapped = audio::playback::RemapFramePosition(
          static_cast<std::size_t>(previousClockFrame), previousSampleRate,
          targetSampleRate);
      audioClockFrameCursor.store(
          static_cast<std::int64_t>(std::min<std::size_t>(
              remapped,
              static_cast<std::size_t>(
                  std::numeric_limits<std::int64_t>::max()))),
          std::memory_order_release);
    }
    currentSampleRate.store(targetSampleRate, std::memory_order_release);
  }

  runtimeState_ = candidate->runtimeState();
  backend =
      std::make_unique<ConfigurableBackendLifecycle>(std::move(candidate));
  backendState.store(audio::playback::BackendRunState::Running,
                     std::memory_order_release);
  openRealtimeSoundGate();
  return true;
}

bool AudioWrapper::restore(const audio::RuntimeState &previous,
                           std::string &errorMessage) {
  return restart(previous.request, errorMessage);
}

void AudioWrapper::setBassBoost(float db) {
  std::lock_guard<std::mutex> lock(soundDataListMutex); // Protect filter coeffs

  // Get current sample rate from backend or default
  int rate = backend ? backend->outputSampleRate() : 44100;
  if (rate == 0)
    rate = 44100;

  // Low Shelf at 100Hz
  bassFilter.setLowShelf((float)rate, 100.0f, db);
}

void AudioWrapper::setTrebleBoost(float db) {
  std::lock_guard<std::mutex> lock(soundDataListMutex); // Protect filter coeffs

  // Get current sample rate
  int rate = backend ? backend->outputSampleRate() : 44100;
  if (rate == 0)
    rate = 44100;

  // High Shelf at 3000Hz (or user preference 8-10kHz? 3kHz is mid-high, let's
  // say 4kHz)
  trebleFilter.setHighShelf((float)rate, 4000.0f, db);
}

void AudioWrapper::setReverbMix(float mix) {
  std::lock_guard<std::mutex> lock(soundDataListMutex);

  int rate = backend ? backend->outputSampleRate() : 44100;
  if (rate == 0)
    rate = 44100;

  if (!reverb.initialized) {
    reverb.init(rate);
  }
  reverb.setMix(mix);
}

void AudioWrapper::setCompressor(float threshold, float ratio) {
  std::lock_guard<std::mutex> lock(soundDataListMutex);

  int rate = backend ? backend->outputSampleRate() : 44100;
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

void AudioWrapper::setVolumes(const audio::Volumes &volumes) {
  bgmGain.store(audio::EffectiveGain(audio::Bus::Bgm, volumes),
                std::memory_order_release);
  keysoundGain.store(audio::EffectiveGain(audio::Bus::Keysound, volumes),
                     std::memory_order_release);
}

void AudioWrapper::setVolumes(const player_settings::AudioSettings &settings) {
  setVolumes(audio::VolumesFromSettings(settings));
}
