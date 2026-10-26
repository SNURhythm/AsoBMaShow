#include "AudioMix.h"
#include "AudioFrameTiming.h"
#include "PcmResampler.h"
#include "../perf/LatencyTelemetry.h"
#include "../settings/AudioVideoSettings.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <utility>

AudioCallbackState::AudioCallbackState()
    : playingSounds(std::make_unique<PlayingSound[]>(kMaxActiveSounds)),
      scheduledSounds(kInitialScheduledSoundCapacity),
      commandQueue(
          std::make_unique<AudioCommand[]>(kCombinedAudioCommandQueueSize)),
      realtimeCommandQueue(
          std::make_unique<AudioCommand[]>(kRealtimeAudioCommandQueueSize)) {
  activeNonSystemVoices.store(0, std::memory_order_relaxed);
  scheduledNonSystemSounds.store(0, std::memory_order_relaxed);
}

namespace audio {
namespace {

float ClampVolume(float value) {
  if (!std::isfinite(value)) {
    return 1.0f;
  }
  return std::clamp(value, 0.0f, 1.0f);
}

std::uint32_t outputOffsetForStartMicros(long long startMicros,
                                         std::int64_t bufferStartFrame,
                                         int sampleRate,
                                         std::uint32_t frameCount,
                                         int playbackRatePercent, bool &isDue) {
  const auto startFrame = frame_timing::at(startMicros, sampleRate, playbackRatePercent);
  isDue = true;
  if (startFrame <= bufferStartFrame) return 0;
  const auto offset = static_cast<std::uint64_t>(startFrame) - static_cast<std::uint64_t>(bufferStartFrame);
  if (offset >= frameCount) {
    isDue = false;
    return 0;
  }
  return static_cast<std::uint32_t>(offset);
}

constexpr std::uint64_t kQ32One = std::uint64_t{1} << 32;
constexpr std::uint64_t kQ32FractionMask = kQ32One - 1;
constexpr size_t kQ32MaximumWholeFrame =
    static_cast<size_t>(std::numeric_limits<std::uint64_t>::max() >> 32);

std::uint64_t frameToQ32(size_t frame) {
  return static_cast<std::uint64_t>(frame) << 32;
}

std::uint64_t remapQ32(std::uint64_t position, int previousSampleRate,
                       int targetSampleRate) {
  if (position == 0 || previousSampleRate <= 0 || targetSampleRate <= 0 ||
      previousSampleRate == targetSampleRate) {
    return position;
  }
#if defined(__SIZEOF_INT128__)
  const __int128 remapped =
      (static_cast<__int128>(position) * targetSampleRate +
       previousSampleRate / 2) /
      previousSampleRate;
  return static_cast<std::uint64_t>(std::min(
      remapped,
      static_cast<__int128>(std::numeric_limits<std::uint64_t>::max())));
#else
  const long double remapped = static_cast<long double>(position) *
                               targetSampleRate / previousSampleRate;
  if (remapped >=
      static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return static_cast<std::uint64_t>(std::floor(remapped + 0.5L));
#endif
}

std::uint64_t rateIncrementQ32(int playbackRatePercent) {
  return frame_timing::increment(playbackRatePercent);
}

bool scheduledSoundLess(const ScheduledSound &lhs, const ScheduledSound &rhs) {
  if (lhs.startMicros != rhs.startMicros) {
    return lhs.startMicros < rhs.startMicros;
  }
  return lhs.sequence < rhs.sequence;
}

void removeActiveSoundAt(AudioCallbackState &state, size_t index) {
  SoundData *soundData = state.playingSounds[index].soundData;
  if (soundData) {
    soundData->playing = false;
  }
  if (state.playingSounds[index].bus != Bus::System) {
    state.activeNonSystemVoices.fetch_sub(1, std::memory_order_release);
  }
  --state.playingSoundCount;
  if (index < state.playingSoundCount) {
    state.playingSounds[index] = state.playingSounds[state.playingSoundCount];
  }
}

// Read stable metadata once per voice/block. A paged owner with no metadata
// is pending, whereas an absent owner keeps the contiguous gameplay path.
struct OutputPcmView {
  const PagedPcmSound::Metadata *paged = nullptr;
  const short *contiguous = nullptr;
  size_t frameCount = 0;
  int channels = 0;
  bool pending = false;

  template<bool Paged>
  const short *frameSamples(size_t frame) const noexcept {
    if constexpr (!Paged) return contiguous + frame * static_cast<size_t>(channels);
    const short *page = paged->pageSamples(frame / PagedPcmSound::kPageFrames);
    return page == nullptr ? nullptr
                           : page + (frame % PagedPcmSound::kPageFrames) *
                                        static_cast<size_t>(channels);
  }
};

OutputPcmView outputPcmView(const SoundData *sound) noexcept {
  if (sound == nullptr) return {};
  if (sound->pagedOutput) {
    const auto *metadata = sound->pagedOutput->metadata();
    if (metadata == nullptr) return {.pending = true};
    return {.paged = metadata, .frameCount = metadata->frameCount,
            .channels = metadata->channels};
  }
  return {.contiguous = sound->outputData.data(),
          .frameCount = sound->outputFrameCount, .channels = sound->channels};
}

bool voiceMixReady(const OutputPcmView &source, std::uint64_t positionQ32,
                   std::uint32_t outputOffsetFrames, bool loop,
                   std::uint32_t frameCount, std::uint64_t increment) noexcept {
  if (source.pending) return false;
  if (source.paged == nullptr || source.channels <= 0 ||
      static_cast<size_t>(positionQ32 >> 32) >= source.frameCount) return true;
  const auto loopLengthQ32 = frameToQ32(source.frameCount);
  std::uint64_t remaining = frameCount - std::min(outputOffsetFrames, frameCount);
  while (remaining != 0) {
    if (loop && positionQ32 >= loopLengthQ32) positionQ32 %= loopLengthQ32;
    const size_t left = static_cast<size_t>(positionQ32 >> 32);
    if (left >= source.frameCount) break;

    // Usual playback rates cannot skip a complete PCM page. Check the page
    // interval for a monotonic run rather than revisiting two atomics for each
    // output frame. Split at sound ends/loop wraps and Q32 overflow exactly as
    // the mixer does. Huge rates use single samples to avoid requiring pages
    // skipped by playback.
    std::uint64_t run = 1;
    if (increment <= PagedPcmSound::kPageFrames * kQ32One) {
      run = std::min(remaining, (loopLengthQ32 - 1 - positionQ32) / increment + 1);
      run = std::min(run,
          (std::numeric_limits<std::uint64_t>::max() - positionQ32) / increment + 1);
    }
    const auto lastPosition = positionQ32 + (run - 1) * increment;
    const size_t lastLeft = static_cast<size_t>(lastPosition >> 32);
    const size_t lastRight = std::min(lastLeft + 1, source.frameCount - 1);
    const size_t firstPage = left / PagedPcmSound::kPageFrames;
    const size_t lastPage = lastRight / PagedPcmSound::kPageFrames;
    for (size_t page = firstPage; page <= lastPage; ++page) {
      if (source.paged->pageSamples(page) == nullptr) return false;
    }
    // Every frame of this sound uses this one immutable page, including loops.
    if (source.paged->pageCount == 1) return true;
    remaining -= run;
    if (lastPosition > std::numeric_limits<std::uint64_t>::max() - increment) break;
    positionQ32 = lastPosition + increment;
    if (positionQ32 >= loopLengthQ32) {
      if (!loop) break;
      positionQ32 %= loopLengthQ32;
    }
  }
  return true;
}

size_t clampFrameToSound(size_t frame, const SoundData *soundData) {
  return soundData == nullptr ? frame
                              : std::min(frame, soundData->outputFrameCount);
}

} // namespace

float EffectiveGain(Bus bus, const Volumes &volumes) {
  const float busVolume = bus == Bus::Bgm
                              ? ClampVolume(volumes.bgm)
                              : bus == Bus::Keysound
                                    ? ClampVolume(volumes.keysound)
                                    : 1.0F;
  return ClampVolume(volumes.master) * busVolume;
}

Volumes VolumesFromSettings(const player_settings::AudioSettings &settings) {
  return {.master = settings.masterVolume,
          .bgm = settings.bgmVolume,
          .keysound = settings.keysoundVolume};
}

std::optional<std::size_t>
ProjectedResampledPcmSampleCount(std::size_t sourceSamples, int channels,
                                 int sourceRate, int targetRate) noexcept {
  if (channels <= 0 || sourceRate <= 0 || targetRate <= 0) return std::nullopt;
  const size_t channelCount = static_cast<size_t>(channels);
  if (sourceSamples % channelCount != 0) return std::nullopt;
  const auto targetFrames = pcm_resampling::Ratio(sourceRate, targetRate).outputFrames(
      sourceSamples / channelCount,
      std::numeric_limits<size_t>::max() / channelCount);
  if (!targetFrames) return std::nullopt;
  return *targetFrames * channelCount;
}

bool ResampledPcmFitsSampleBudget(
    std::size_t sourceSamples, int channels, int sourceRate, int targetRate,
    std::size_t maximumCombinedSamples) noexcept {
  const auto outputSamples = ProjectedResampledPcmSampleCount(
      sourceSamples, channels, sourceRate, targetRate);
  return outputSamples && sourceSamples <= maximumCombinedSamples &&
         *outputSamples <= maximumCombinedSamples - sourceSamples;
}

std::vector<short> ResamplePcm(std::span<const short> source, int channels,
                               int sourceRate, int targetRate,
                               const std::atomic_bool *cancelled) {
  const auto projectedSamples = ProjectedResampledPcmSampleCount(
      source.size(), channels, sourceRate, targetRate);
  if (!projectedSamples || source.empty()) return {};
  if (cancelled && cancelled->load(std::memory_order_relaxed)) return {};
  if (sourceRate == targetRate && !cancelled) return {source.begin(), source.end()};

  const size_t channelCount = static_cast<size_t>(channels);
  const size_t sourceFrames = source.size() / channelCount;
  const size_t targetFrames = *projectedSamples / channelCount;
  std::vector<short> output(*projectedSamples);

  pcm_resampling::Cursor cursor(
      pcm_resampling::Ratio(sourceRate, targetRate), sourceFrames);
  for (size_t targetFrame = 0; targetFrame < targetFrames;) {
    // Whole-asset preview preparation must remain interruptible, including
    // cancellation while allocating its output and same-rate PCM copies.
    if (cancelled && cancelled->load(std::memory_order_relaxed)) return {};
    const size_t chunkFrames = std::min<size_t>(4096, targetFrames - targetFrame);
    const size_t chunkEnd = targetFrame + chunkFrames;
    if (sourceRate == targetRate) {
      const size_t offset = targetFrame * channelCount;
      std::copy_n(source.data() + offset, chunkFrames * channelCount, output.data() + offset);
      targetFrame = chunkEnd;
      continue;
    }
    for (; targetFrame < chunkEnd; ++targetFrame) {
      const size_t leftIndex = cursor.left() * channelCount;
      const size_t rightIndex = cursor.right() * channelCount;
      for (size_t channel = 0; channel < channelCount; ++channel) {
        output[targetFrame * channelCount + channel] = cursor.interpolate(
            source[leftIndex + channel], source[rightIndex + channel]);
      }
      cursor.advance();
    }
  }

  return output;
}

namespace playback {

BackendOperationResult
ConfirmBackendStopped(IBackendLifecycle &backend,
                      const BackendStateObservation &initialState,
                      std::atomic<BackendRunState> &backendState) {
  backendState.store(initialState.state, std::memory_order_release);
  if (initialState.state == BackendRunState::Unknown) {
    return {.success = false,
            .diagnostic = initialState.diagnostic.empty()
                              ? "Unable to determine audio backend state"
                              : initialState.diagnostic};
  }
  if (initialState.state == BackendRunState::Stopped) {
    return {.success = true};
  }

  const BackendOperationResult stopped = backend.stopAndDrain();
  if (!stopped.success) {
    backendState.store(BackendRunState::Unknown, std::memory_order_release);
    return {.success = false,
            .diagnostic = stopped.diagnostic.empty()
                              ? "Unable to stop and drain audio backend"
                              : stopped.diagnostic};
  }

  const BackendStateObservation afterStop = backend.observeState();
  backendState.store(afterStop.state, std::memory_order_release);
  if (afterStop.state != BackendRunState::Stopped) {
    return {
        .success = false,
        .diagnostic =
            afterStop.diagnostic.empty()
                ? "Audio backend did not confirm a stopped state after drain"
                : afterStop.diagnostic,
    };
  }
  return {.success = true};
}

BackendStateObservation InterpretStoppedQueryResult(int result,
                                                    std::string diagnostic) {
  if (result == 1) {
    return {.state = BackendRunState::Stopped};
  }
  if (result == 0) {
    return {.state = BackendRunState::Running};
  }
  return {.state = BackendRunState::Unknown,
          .diagnostic = diagnostic.empty() ? "Audio backend state query failed"
                                           : std::move(diagnostic)};
}

bool CanMutateCallbackStateDirectly(BackendRunState state) noexcept {
  return state == BackendRunState::Stopped;
}

BackendOperationResult EnsureBackendStartedAtOutputRate(
    IBackendLifecycle &backend, std::span<SoundData *const> sounds,
    AudioCallbackState &callbackState, int targetSampleRate,
    std::atomic<int> &currentSampleRate,
    std::atomic<int64_t> &audioClockFrameCursor,
    std::atomic<BackendRunState> &backendState) {
  const BackendStateObservation observed = backend.observeState();
  if (observed.state == BackendRunState::Unknown) {
    return ConfirmBackendStopped(backend, observed, backendState);
  }

  backendState.store(observed.state, std::memory_order_release);
  if (targetSampleRate <= 0) {
    targetSampleRate = 44100;
  }
  const int previousSampleRate =
      currentSampleRate.load(std::memory_order_acquire);
  if (observed.state == BackendRunState::Running &&
      previousSampleRate == targetSampleRate) {
    return {.success = true};
  }

  const BackendOperationResult stopped =
      ConfirmBackendStopped(backend, observed, backendState);
  if (!stopped.success) {
    return stopped;
  }

  if (previousSampleRate != targetSampleRate) {
    auto transition = PrepareOutputRateTransition(sounds, previousSampleRate,
                                                  targetSampleRate);
    if (!transition.has_value()) {
      return {.success = false,
              .diagnostic = "Unable to prepare PCM for output-rate change"};
    }

    CommitOutputRateTransition(std::move(*transition), callbackState);
    const int64_t clockFrame =
        audioClockFrameCursor.load(std::memory_order_acquire);
    if (clockFrame > 0) {
      const size_t remappedClockFrame =
          RemapFramePosition(static_cast<size_t>(clockFrame),
                             previousSampleRate, targetSampleRate);
      audioClockFrameCursor.store(
          static_cast<int64_t>(std::min<size_t>(
              remappedClockFrame,
              static_cast<size_t>(std::numeric_limits<int64_t>::max()))),
          std::memory_order_release);
    }
    currentSampleRate.store(targetSampleRate, std::memory_order_release);
  }

  const BackendOperationResult started = backend.start();
  if (!started.success) {
    backendState.store(BackendRunState::Unknown, std::memory_order_release);
    return {.success = false,
            .diagnostic = started.diagnostic.empty()
                              ? "Unable to start audio backend"
                              : started.diagnostic};
  }

  const BackendStateObservation afterStart = backend.observeState();
  backendState.store(afterStart.state, std::memory_order_release);
  if (afterStart.state != BackendRunState::Running) {
    return {
        .success = false,
        .diagnostic =
            afterStart.diagnostic.empty()
                ? "Audio backend did not confirm a running state after start"
                : afterStart.diagnostic,
    };
  }
  return {.success = true};
}

BackendOperationResult
StopBackendAndClearCallbackState(IBackendLifecycle &backend,
                                 AudioCallbackState &callbackState,
                                 std::atomic<BackendRunState> &backendState) {
  const BackendOperationResult stopped =
      ConfirmBackendStopped(backend, backend.observeState(), backendState);
  if (!stopped.success) {
    return stopped;
  }
  DrainRealtimeCommands(callbackState);
  DrainCommands(callbackState);
  ClearCallbackSounds(callbackState);
  callbackState.commandReadCursor.store(0, std::memory_order_release);
  callbackState.commandWriteCursor.store(0, std::memory_order_release);
  callbackState.ordinaryCommandCount.store(0, std::memory_order_release);
  callbackState.ownerControlCommandCount.store(0,
                                               std::memory_order_release);
  callbackState.ownerRetirementCommandCount.store(0,
                                                  std::memory_order_release);
  callbackState.realtimeCommandReadCursor.store(0,
                                                std::memory_order_release);
  callbackState.realtimeCommandWriteCursor.store(0,
                                                 std::memory_order_release);
  return {.success = true};
}

size_t RemapFramePosition(size_t frame, int previousSampleRate,
                          int targetSampleRate) {
  if (frame == 0 || previousSampleRate <= 0 || targetSampleRate <= 0 ||
      previousSampleRate == targetSampleRate) {
    return frame;
  }

  const long double remapped =
      static_cast<long double>(frame) * targetSampleRate / previousSampleRate;
  if (remapped >=
      static_cast<long double>(std::numeric_limits<size_t>::max())) {
    return std::numeric_limits<size_t>::max();
  }
  return static_cast<size_t>(std::floor(remapped + 0.5L));
}

std::optional<OutputRateTransition>
PrepareOutputRateTransition(std::span<SoundData *const> sounds,
                            int previousSampleRate, int targetSampleRate) {
  if (previousSampleRate <= 0 || targetSampleRate <= 0) {
    return std::nullopt;
  }

  OutputRateTransition transition{
      .previousSampleRate = previousSampleRate,
      .targetSampleRate = targetSampleRate,
  };
  transition.candidates.reserve(sounds.size());
  std::unordered_map<SoundData *, size_t> preparedFrameCounts;
  preparedFrameCounts.reserve(sounds.size());
  for (SoundData *soundData : sounds) {
    if (soundData == nullptr) {
      continue;
    }
    if (!soundData->layers.empty()) {
      transition.candidates.push_back({.soundData = soundData});
      continue;
    }
    if (soundData->channels <= 0 || soundData->sourceSampleRate <= 0 ||
        soundData->sourceData.size() %
                static_cast<size_t>(soundData->channels) !=
            0) {
      return std::nullopt;
    }

    SharedPcmBuffer outputData = targetSampleRate == soundData->sourceSampleRate
        ? soundData->sourceData
        : SharedPcmBuffer(ResamplePcm(soundData->sourceData, soundData->channels,
                                      soundData->sourceSampleRate, targetSampleRate));
    if (!soundData->sourceData.empty() && outputData.empty()) {
      return std::nullopt;
    }
    const size_t outputFrameCount =
        outputData.size() / static_cast<size_t>(soundData->channels);
    transition.candidates.push_back({.soundData = soundData,
                                     .outputData = std::move(outputData),
                                     .outputFrameCount = outputFrameCount});
    preparedFrameCounts.emplace(soundData, outputFrameCount);
  }
  // Groups contain elementary sounds. Derive their bounds after every layer
  // is prepared, independently of owner order. Scaling an already rounded
  // length can both discard the last audible frame and invent a silent tail.
  for (auto &candidate : transition.candidates) {
    for (const auto &layer : candidate.soundData->layers) {
      const auto prepared = preparedFrameCounts.find(layer.get());
      if (prepared == preparedFrameCounts.end()) return std::nullopt;
      candidate.outputFrameCount = std::max(candidate.outputFrameCount, prepared->second);
    }
  }
  return transition;
}

void CommitOutputRateTransition(OutputRateTransition transition,
                                AudioCallbackState &state) {
  for (auto &candidate : transition.candidates) {
    candidate.soundData->outputData = std::move(candidate.outputData);
    candidate.soundData->outputFrameCount = candidate.outputFrameCount;
    candidate.soundData->currentFrame =
        clampFrameToSound(RemapFramePosition(candidate.soundData->currentFrame,
                                             transition.previousSampleRate,
                                             transition.targetSampleRate),
                          candidate.soundData);
  }

  for (size_t index = 0; index < state.playingSoundCount; ++index) {
    auto &playingSound = state.playingSounds[index];
    playingSound.sourceFrameQ32 =
        remapQ32(playingSound.sourceFrameQ32, transition.previousSampleRate,
                 transition.targetSampleRate);
    const size_t remappedOutputOffset = RemapFramePosition(
        playingSound.outputOffsetFrames, transition.previousSampleRate,
        transition.targetSampleRate);
    playingSound.outputOffsetFrames = static_cast<std::uint32_t>(std::min(
        remappedOutputOffset,
        static_cast<size_t>(std::numeric_limits<std::uint32_t>::max())));
    if (playingSound.outputEndOffsetFrames != std::numeric_limits<std::uint32_t>::max()) {
      playingSound.outputEndOffsetFrames = static_cast<std::uint32_t>(std::min<size_t>(
          RemapFramePosition(playingSound.outputEndOffsetFrames, transition.previousSampleRate,
                             transition.targetSampleRate),
          std::numeric_limits<std::uint32_t>::max()));
    }
  }

  for (size_t index = 0; index < state.scheduledSoundCount; ++index) {
    auto &scheduledSound = state.scheduledSounds[index];
    const auto position = remapQ32(frameToQ32(scheduledSound.startFrame) | scheduledSound.startFractionQ32,
                                    transition.previousSampleRate, transition.targetSampleRate);
    scheduledSound.startFrame = clampFrameToSound(position >> 32, scheduledSound.soundData);
    scheduledSound.startFractionQ32 = scheduledSound.startFrame == (position >> 32)
        ? static_cast<std::uint32_t>(position) : 0;
  }

  const std::uint32_t readCursor =
      state.commandReadCursor.load(std::memory_order_acquire);
  const std::uint32_t writeCursor =
      state.commandWriteCursor.load(std::memory_order_acquire);
  for (std::uint32_t cursor = readCursor; cursor != writeCursor; ++cursor) {
    auto &command = state.commandQueue[cursor % kCombinedAudioCommandQueueSize];
    const auto position = remapQ32(frameToQ32(command.startFrame) | command.startFractionQ32,
                                    transition.previousSampleRate, transition.targetSampleRate);
    command.startFrame = clampFrameToSound(position >> 32, command.soundData);
    command.startFractionQ32 = command.startFrame == (position >> 32)
        ? static_cast<std::uint32_t>(position) : 0;
  }

  const std::uint32_t realtimeReadCursor =
      state.realtimeCommandReadCursor.load(std::memory_order_acquire);
  const std::uint32_t realtimeWriteCursor =
      state.realtimeCommandWriteCursor.load(std::memory_order_acquire);
  for (std::uint32_t cursor = realtimeReadCursor;
       cursor != realtimeWriteCursor; ++cursor) {
    auto &command = state.realtimeCommandQueue[
        cursor % kRealtimeAudioCommandQueueSize];
    const auto position = remapQ32(frameToQ32(command.startFrame) | command.startFractionQ32,
                                    transition.previousSampleRate, transition.targetSampleRate);
    command.startFrame = clampFrameToSound(position >> 32, command.soundData);
    command.startFractionQ32 = command.startFrame == (position >> 32)
        ? static_cast<std::uint32_t>(position) : 0;
  }
}

static bool appendActiveSound(AudioCallbackState &state, SoundData *soundData, Bus bus,
                       std::uint32_t outputOffsetFrames, size_t startFrame,
                       float gain, bool loop, std::uint64_t voiceId, std::uint32_t startFractionQ32,
                       bool &transferControl) {
  if (soundData != nullptr && !soundData->layers.empty()) {
    bool added = false;
    for (const auto &layer : soundData->layers) {
      if (appendActiveSound(state, layer.get(), bus, outputOffsetFrames, startFrame, gain, loop,
                            voiceId, startFractionQ32, transferControl)) {
        state.playingSounds[state.playingSoundCount - 1].groupOwner = soundData;
        added = true;
      }
    }
    return added;
  }
  if (soundData != nullptr && soundData->monophonic) {
    for (std::size_t i = 0; i < state.playingSoundCount;) {
      auto &voice = state.playingSounds[i];
      if (voice.soundData == soundData) {
        if (voice.outputOffsetFrames >= outputOffsetFrames) {
          removeActiveSoundAt(state, i);
          continue;
        }
        // Scheduled retriggers can fall later in this same callback. Preserve
        // the old voice's prefix, then switch to the new slice exactly on time.
        voice.outputEndOffsetFrames = std::min(voice.outputEndOffsetFrames, outputOffsetFrames);
      }
      ++i;
    }
  }
  if (soundData == nullptr || startFrame >= outputPcmView(soundData).frameCount ||
      startFrame > kQ32MaximumWholeFrame ||
      state.playingSoundCount >= kMaxActiveSounds) {
    return false;
  }
  if (transferControl && voiceId != 0 && bus == Bus::Keysound) {
    // Live input identifies a note, which may have several overlapping BMS
    // triggers. Only its latest successful trigger owns later LN gain changes;
    // keep earlier polyphonic voices audible, with their existing gains.
    for (size_t i = 0; i < state.playingSoundCount; ++i) {
      auto &voice = state.playingSounds[i];
      if (voice.voiceId == voiceId && voice.bus == Bus::Keysound) voice.voiceId = 0;
    }
    transferControl = false;
  }
  soundData->playing = true;
  state.playingSounds[state.playingSoundCount++] = {
      .soundData = soundData,
      .bus = bus,
      .sourceFrameQ32 = frameToQ32(startFrame) | startFractionQ32,
      .outputOffsetFrames = outputOffsetFrames,
      .gain = gain,
      .loop = loop,
      .voiceId = voiceId,
  };
  if (bus != Bus::System) {
    state.activeNonSystemVoices.fetch_add(1, std::memory_order_release);
  }
  return true;
}

bool AppendActiveSound(AudioCallbackState &state, SoundData *soundData, Bus bus,
                       std::uint32_t outputOffsetFrames, size_t startFrame,
                       float gain, bool loop, std::uint64_t voiceId, std::uint32_t startFractionQ32) {
  bool transferControl = true;
  return appendActiveSound(state, soundData, bus, outputOffsetFrames, startFrame, gain, loop,
                           voiceId, startFractionQ32, transferControl);
}

static bool appendRealtimeActiveSound(AudioCallbackState &state,
                                      SoundData *soundData, Bus bus,
                                      size_t startFrame, std::uint64_t voiceId,
                                      std::uint32_t startFractionQ32, bool &transferControl) {
  if (soundData != nullptr && !soundData->layers.empty()) {
    bool added = false;
    for (const auto &layer : soundData->layers) {
      if (appendRealtimeActiveSound(state, layer.get(), bus, startFrame, voiceId,
                                    startFractionQ32, transferControl)) {
        state.playingSounds[state.playingSoundCount - 1].groupOwner = soundData;
        added = true;
      }
    }
    return added;
  }
  if (soundData == nullptr || startFrame >= outputPcmView(soundData).frameCount ||
      startFrame > kQ32MaximumWholeFrame) {
    return false;
  }
  if (state.playingSoundCount >= kMaxActiveSounds &&
      !(soundData->monophonic && std::any_of(
          state.playingSounds.get(), state.playingSounds.get() + state.playingSoundCount,
          [soundData](const PlayingSound &voice) { return voice.soundData == soundData; }))) {
    // A realtime slice retrigger replaces its existing voice at offset zero.
    // Only steal another keysound when the append actually needs a new slot.
    std::size_t preemptIndex = 0;
    while (preemptIndex < state.playingSoundCount &&
           state.playingSounds[preemptIndex].bus != Bus::Keysound) {
      ++preemptIndex;
    }
    if (preemptIndex == state.playingSoundCount) {
      return false;
    }
    removeActiveSoundAt(state, preemptIndex);
  }
  return appendActiveSound(state, soundData, bus, 0, startFrame, 1.0F, false,
                           voiceId, startFractionQ32, transferControl);
}

static bool AppendRealtimeActiveSound(AudioCallbackState &state, SoundData *soundData, Bus bus,
                                      size_t startFrame, std::uint64_t voiceId,
                                      std::uint32_t startFractionQ32) {
  bool transferControl = true;
  return appendRealtimeActiveSound(state, soundData, bus, startFrame, voiceId,
                                   startFractionQ32, transferControl);
}

// A realtime gain command touches only currently owned voices, never a sound
// resource or its next trigger. Layers retain their initiating note's identity.
static void SetVoiceGain(AudioCallbackState &state, std::uint64_t voiceId, float gain) {
  if (voiceId == 0) return;
  for (std::size_t i = 0; i < state.playingSoundCount; ++i) {
    auto &voice = state.playingSounds[i];
    if (voice.voiceId == voiceId && voice.bus == Bus::Keysound) voice.gain = gain;
  }
}

bool PrepareScheduledSoundCapacity(AudioCallbackState &state,
                                   size_t requiredCapacity) noexcept {
  if (requiredCapacity <= state.scheduledSoundCapacity) return true;
  // The live non-system count is uint32_t; retain its representation bound.
  constexpr size_t maximumCapacity = std::min<size_t>(
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<size_t>::max() / sizeof(ScheduledSound));
  if (requiredCapacity > maximumCapacity) return false;
  const size_t grownCapacity = state.scheduledSoundCapacity > maximumCapacity / 2
                                   ? maximumCapacity
                                   : state.scheduledSoundCapacity * 2;
  const size_t capacity = std::max(requiredCapacity, grownCapacity);
  try {
    ScheduledSoundBuffer replacement(capacity);
    for (size_t index = 0; index < state.scheduledSoundCount; ++index) {
      replacement[index] = state.scheduledSounds[index];
    }
    state.scheduledSounds = std::move(replacement);
    state.scheduledSoundCapacity = capacity;
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

bool InsertScheduledSound(AudioCallbackState &state,
                          const ScheduledSound &scheduledSound) {
  if (scheduledSound.soundData == nullptr ||
      state.scheduledSoundCount >= state.scheduledSoundCapacity) {
    return false;
  }

  scheduledSound.soundData->hasBeenScheduled = true;
  if (state.scheduledSoundCount == 0 ||
      !scheduledSoundLess(
          scheduledSound,
          state.scheduledSounds[state.scheduledSoundCount - 1])) {
    state.scheduledSounds[state.scheduledSoundCount++] = scheduledSound;
    if (scheduledSound.bus != Bus::System) {
      state.scheduledNonSystemSounds.fetch_add(1, std::memory_order_release);
    }
    return true;
  }

  size_t insertIndex = 0;
  while (
      insertIndex < state.scheduledSoundCount &&
      !scheduledSoundLess(scheduledSound, state.scheduledSounds[insertIndex])) {
    ++insertIndex;
  }
  for (size_t index = state.scheduledSoundCount; index > insertIndex; --index) {
    state.scheduledSounds[index] = state.scheduledSounds[index - 1];
  }
  state.scheduledSounds[insertIndex] = scheduledSound;
  ++state.scheduledSoundCount;
  if (scheduledSound.bus != Bus::System) {
    state.scheduledNonSystemSounds.fetch_add(1, std::memory_order_release);
  }
  return true;
}

void ClearCallbackSounds(AudioCallbackState &state, bool preserveSystemSounds) {
  state.scheduledMixPending = false;
  for (size_t index = 0; index < state.playingSoundCount;) {
    if (preserveSystemSounds && state.playingSounds[index].bus == Bus::System) {
      ++index;
    } else {
      removeActiveSoundAt(state, index);
    }
  }
  size_t retained = 0;
  if (preserveSystemSounds) {
    for (size_t index = 0; index < state.scheduledSoundCount; ++index) {
      if (state.scheduledSounds[index].bus == Bus::System) {
        state.scheduledSounds[retained++] = state.scheduledSounds[index];
      }
    }
  }
  state.scheduledSoundCount = retained;
  if (retained == 0) {
    state.scheduledSounds.reset();
  }
  state.activeNonSystemVoices.store(0, std::memory_order_release);
  state.scheduledNonSystemSounds.store(0, std::memory_order_release);
}

void RemoveSound(AudioCallbackState &state, SoundData *soundData) {
  std::size_t active = 0;
  while (active < state.playingSoundCount) {
    if (state.playingSounds[active].soundData == soundData ||
        state.playingSounds[active].groupOwner == soundData) {
      removeActiveSoundAt(state, active);
    } else {
      ++active;
    }
  }
  // Private skin voices usually only play immediately. Their stop/retirement
  // must not walk an unrelated chart's entire preloaded schedule on the callback.
  if (soundData != nullptr && !soundData->hasBeenScheduled) {
    return;
  }
  std::size_t retained = 0;
  std::uint32_t removedScheduledNonSystem = 0;
  for (std::size_t index = 0; index < state.scheduledSoundCount; ++index) {
    if (state.scheduledSounds[index].soundData == soundData) {
      if (state.scheduledSounds[index].bus != Bus::System) {
        ++removedScheduledNonSystem;
      }
      continue;
    }
    state.scheduledSounds[retained++] = state.scheduledSounds[index];
  }
  state.scheduledSoundCount = retained;
  if (removedScheduledNonSystem > 0) {
    state.scheduledNonSystemSounds.fetch_sub(removedScheduledNonSystem,
                                              std::memory_order_release);
  }
}

bool EnqueueCommand(AudioCallbackState &state, const AudioCommand &command,
                    std::uint64_t *submissionSequence) {
  const std::uint32_t readCursor =
      state.commandReadCursor.load(std::memory_order_acquire);
  const std::uint32_t writeCursor =
      state.commandWriteCursor.load(std::memory_order_relaxed);
  if (state.ordinaryCommandCount.load(std::memory_order_acquire) >=
          kAudioCommandQueueSize ||
      writeCursor - readCursor >= kCombinedAudioCommandQueueSize) {
    return false;
  }

  AudioCommand submitted = command;
  submitted.submissionSequence =
      state.nextCommandSubmissionSequence.fetch_add(1,
                                                    std::memory_order_relaxed);
  submitted.admission = AudioCommandAdmission::Ordinary;
  state.commandQueue[writeCursor % kCombinedAudioCommandQueueSize] = submitted;
  state.ordinaryCommandCount.fetch_add(1, std::memory_order_release);
  state.commandWriteCursor.store(writeCursor + 1, std::memory_order_release);
  if (submissionSequence != nullptr) {
    *submissionSequence = submitted.submissionSequence;
  }
  return true;
}

bool EnqueueOwnerControlCommand(AudioCallbackState &state,
                                const AudioCommand &command,
                                std::uint64_t *submissionSequence) {
  const std::uint32_t readCursor =
      state.commandReadCursor.load(std::memory_order_acquire);
  const std::uint32_t writeCursor =
      state.commandWriteCursor.load(std::memory_order_relaxed);
  if (state.ownerControlCommandCount.load(std::memory_order_acquire) >=
          kOwnerControlCommandQueueSize ||
      writeCursor - readCursor >= kCombinedAudioCommandQueueSize) {
    return false;
  }
  AudioCommand submitted = command;
  submitted.submissionSequence =
      state.nextCommandSubmissionSequence.fetch_add(1,
                                                    std::memory_order_relaxed);
  submitted.admission = AudioCommandAdmission::OwnerControl;
  state.commandQueue[writeCursor % kCombinedAudioCommandQueueSize] = submitted;
  state.ownerControlCommandCount.fetch_add(1, std::memory_order_release);
  state.commandWriteCursor.store(writeCursor + 1, std::memory_order_release);
  if (submissionSequence != nullptr) {
    *submissionSequence = submitted.submissionSequence;
  }
  return true;
}

bool EnqueueOwnerRetirementCommand(
    AudioCallbackState &state, const AudioCommand &command,
    std::uint64_t *submissionSequence) {
  const std::uint32_t readCursor =
      state.commandReadCursor.load(std::memory_order_acquire);
  const std::uint32_t writeCursor =
      state.commandWriteCursor.load(std::memory_order_relaxed);
  if (state.ownerRetirementCommandCount.load(std::memory_order_acquire) >=
          kOwnerControlCommandQueueSize ||
      writeCursor - readCursor >= kCombinedAudioCommandQueueSize) {
    return false;
  }
  AudioCommand submitted = command;
  submitted.submissionSequence =
      state.nextCommandSubmissionSequence.fetch_add(1,
                                                    std::memory_order_relaxed);
  submitted.admission = AudioCommandAdmission::OwnerRetirement;
  state.commandQueue[writeCursor % kCombinedAudioCommandQueueSize] = submitted;
  state.ownerRetirementCommandCount.fetch_add(1,
                                              std::memory_order_release);
  state.commandWriteCursor.store(writeCursor + 1, std::memory_order_release);
  if (submissionSequence != nullptr) {
    *submissionSequence = submitted.submissionSequence;
  }
  return true;
}

std::optional<RealtimeAudioCommandReservation>
TryReserveRealtimeCommand(const AudioCallbackState &state) noexcept {
  const std::uint32_t readCursor =
      state.realtimeCommandReadCursor.load(std::memory_order_acquire);
  const std::uint32_t writeCursor =
      state.realtimeCommandWriteCursor.load(std::memory_order_relaxed);
  if (writeCursor - readCursor >= kRealtimeAudioCommandQueueSize) {
    return std::nullopt;
  }
  return RealtimeAudioCommandReservation{.cursor = writeCursor};
}

bool CommitRealtimeCommand(
    AudioCallbackState &state, RealtimeAudioCommandReservation reservation,
    const AudioCommand &command) noexcept {
  const std::uint32_t writeCursor =
      state.realtimeCommandWriteCursor.load(std::memory_order_relaxed);
  if (writeCursor != reservation.cursor) {
    return false;
  }
  state.realtimeCommandQueue[writeCursor % kRealtimeAudioCommandQueueSize] =
      command;
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
  state.realtimeCommandQueue[writeCursor % kRealtimeAudioCommandQueueSize]
      .submittedSteadyMicros = perf::latency::nowMicros();
#endif
  state.realtimeCommandWriteCursor.store(writeCursor + 1,
                                         std::memory_order_release);
  return true;
}

void DrainRealtimeCommands(AudioCallbackState &state, bool rendering) noexcept {
#if !ASOBMASHOW_ENABLE_PERF_TELEMETRY
  (void)rendering;
#endif
  std::uint32_t readCursor =
      state.realtimeCommandReadCursor.load(std::memory_order_relaxed);
  const std::uint32_t writeCursor =
      state.realtimeCommandWriteCursor.load(std::memory_order_acquire);
  while (readCursor != writeCursor) {
    const AudioCommand &command = state.realtimeCommandQueue[
        readCursor % kRealtimeAudioCommandQueueSize];
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
    if (rendering && command.submittedSteadyMicros > 0) {
      const auto age = perf::latency::nowMicros() - command.submittedSteadyMicros;
      if (age >= 0) perf::latency::record(
          perf::latency::Stage::SoundCommandToCallback, static_cast<std::uint64_t>(age));
    }
#endif
    switch (command.type) {
    case AudioCommandType::PlayNow:
      AppendRealtimeActiveSound(state, command.soundData, command.bus,
                                command.startFrame, command.voiceId, command.startFractionQ32);
      break;
    case AudioCommandType::Schedule:
      InsertScheduledSound(state, {.soundData = command.soundData,
                                   .bus = command.bus,
                                   .startMicros = command.startMicros,
                                   .sequence = command.sequence,
                                   .startFrame = command.startFrame,
                                   .startFractionQ32 = command.startFractionQ32,
                                   .gain = command.gain,
                                   .loop = command.loop,
                                   .voiceId = command.voiceId});
      break;
    case AudioCommandType::SetVoiceGain:
      SetVoiceGain(state, command.voiceId, command.gain);
      break;
    case AudioCommandType::StopOwner:
      RemoveSound(state, command.soundData);
      if (command.soundData != nullptr) {
        command.soundData->ownerControlAcknowledgedSequence.store(
            command.submissionSequence, std::memory_order_release);
      }
      if (command.acknowledgement != nullptr) {
        command.acknowledgement->store(true, std::memory_order_release);
      }
      break;
    case AudioCommandType::StopAll:
      ClearCallbackSounds(state);
      break;
    }
    ++readCursor;
  }
  state.realtimeCommandReadCursor.store(readCursor,
                                        std::memory_order_release);
}

static void ApplyCommand(AudioCallbackState &state,
                         const AudioCommand &command) noexcept {
  switch (command.type) {
  case AudioCommandType::PlayNow:
    AppendActiveSound(state, command.soundData, command.bus, 0,
                      command.startFrame, command.gain, command.loop, command.voiceId, command.startFractionQ32);
    break;
  case AudioCommandType::Schedule:
    InsertScheduledSound(state, {.soundData = command.soundData,
                                 .bus = command.bus,
                                 .startMicros = command.startMicros,
                                 .sequence = command.sequence,
                                 .startFrame = command.startFrame,
                                   .startFractionQ32 = command.startFractionQ32,
                                 .gain = command.gain,
                                 .loop = command.loop,
                                   .voiceId = command.voiceId});
    break;
  case AudioCommandType::SetVoiceGain:
    SetVoiceGain(state, command.voiceId, command.gain);
    break;
  case AudioCommandType::StopOwner:
    RemoveSound(state, command.soundData);
    if (command.soundData != nullptr) {
      command.soundData->ownerControlAcknowledgedSequence.store(
          command.submissionSequence, std::memory_order_release);
    }
    if (command.acknowledgement != nullptr) {
      command.acknowledgement->store(true, std::memory_order_release);
    }
    break;
  case AudioCommandType::StopAll:
    ClearCallbackSounds(state);
    break;
  }
}

void DrainCommands(AudioCallbackState &state,
                   CommandDrainSnapshotHook afterSnapshot,
                   void *hookContext) {
  std::uint32_t readCursor =
      state.commandReadCursor.load(std::memory_order_relaxed);
  const std::uint32_t writeCursor =
      state.commandWriteCursor.load(std::memory_order_acquire);
  if (afterSnapshot != nullptr) {
    afterSnapshot(hookContext);
  }

  while (readCursor != writeCursor) {
    const AudioCommand &command = state.commandQueue[
        readCursor++ % kCombinedAudioCommandQueueSize];
    ApplyCommand(state, command);
    if (command.admission == AudioCommandAdmission::OwnerControl) {
      state.ownerControlCommandCount.fetch_sub(1, std::memory_order_release);
    } else if (command.admission == AudioCommandAdmission::OwnerRetirement) {
      state.ownerRetirementCommandCount.fetch_sub(1,
                                                  std::memory_order_release);
    } else {
      state.ordinaryCommandCount.fetch_sub(1, std::memory_order_release);
    }
  }

  state.commandReadCursor.store(readCursor, std::memory_order_release);
}

bool ScheduledMixReady(const AudioCallbackState &state,
                        long long bufferStartMicros, int sampleRate,
                        std::uint32_t frameCount, int playbackRatePercent,
                        std::uint32_t bufferOffsetFrames, bool includeAllDueSounds,
                        std::optional<std::int64_t> bufferStartFrame) noexcept {
  if (frameCount > std::numeric_limits<std::uint32_t>::max() - bufferOffsetFrames) {
    return false;
  }
  // Only plain cursors are copied: no PCM ownership, allocation or publication.
  // Simulate the same boundaries as MixActiveSounds, so a replacement at full
  // capacity and a new start after retirement still require their paged PCM.
  std::array<PlayingSound, kMaxActiveSounds> voices;
  size_t count = state.playingSoundCount;
  std::copy_n(state.playingSounds.get(), count, voices.begin());
  const auto increment = rateIncrementQ32(playbackRatePercent);
  const auto advance = [&](std::uint32_t frames) {
    for (size_t i = 0; i < count;) {
      auto &voice = voices[i];
      const auto source = outputPcmView(voice.soundData);
      const auto end = std::min(frames, voice.outputEndOffsetFrames);
      if (!voiceMixReady(source, voice.sourceFrameQ32, voice.outputOffsetFrames,
                         voice.loop, end, increment)) return false;
      const auto start = std::min(frames, voice.outputOffsetFrames);
      const auto played = end > start ? end - start : 0;
      bool finished = source.channels <= 0 || source.frameCount == 0 ||
          (voice.sourceFrameQ32 >> 32) >= source.frameCount ||
          voice.outputEndOffsetFrames <= frames;
      if (!finished && played > 0) {
        const auto length = frameToQ32(source.frameCount);
        auto remaining = static_cast<std::uint64_t>(played);
        while (remaining > 0) {
          const auto position = voice.sourceFrameQ32;
          const auto safe = (std::numeric_limits<std::uint64_t>::max() - position) / increment;
          if (safe == 0) { finished = true; break; }
          const auto untilEnd = (length - 1 - position) / increment + 1;
          const auto run = std::min({remaining, safe, untilEnd});
          voice.sourceFrameQ32 += run * increment;
          remaining -= run;
          if (voice.sourceFrameQ32 >= length) {
            if (!voice.loop) { finished = true; break; }
            voice.sourceFrameQ32 %= length;
          }
        }
      }
      if (finished) { voices[i] = voices[--count]; continue; }
      voice.outputOffsetFrames -= start;
      if (voice.outputEndOffsetFrames != std::numeric_limits<std::uint32_t>::max())
        voice.outputEndOffsetFrames -= frames;
      ++i;
    }
    return true;
  };
  const auto admit = [&](auto &&self, const ScheduledSound &event,
                          SoundData *sound, std::uint32_t remaining) -> bool {
    if (sound == nullptr) return true;
    if (!sound->layers.empty()) {
      for (const auto &layer : sound->layers)
        if (!self(self, event, layer.get(), remaining)) return false;
      return true;
    }
    if (sound->monophonic) {
      for (size_t i = 0; i < count;)
        if (voices[i].soundData == sound) voices[i] = voices[--count];
        else ++i;
    }
    if (count >= kMaxActiveSounds && !includeAllDueSounds) return true;
    if (event.startFrame > kQ32MaximumWholeFrame) return true;
    const auto source = outputPcmView(sound);
    if (source.pending) return false;
    if (event.startFrame >= source.frameCount) return true;
    if (count >= kMaxActiveSounds)
      return voiceMixReady(source, (frameToQ32(event.startFrame) | event.startFractionQ32), 0,
                           event.loop, remaining, increment);
    voices[count++] = {.soundData = sound, .sourceFrameQ32 = (frameToQ32(event.startFrame) | event.startFractionQ32),
                       .loop = event.loop};
    return true;
  };
  std::uint32_t cursor = 0;
  for (size_t index = 0; index < state.scheduledSoundCount; ++index) {
    const auto &event = state.scheduledSounds[index];
    bool due = false;
    const auto absolute = outputOffsetForStartMicros(event.startMicros,
        bufferStartFrame.value_or(frame_timing::at(bufferStartMicros, sampleRate, playbackRatePercent)),
        sampleRate, frameCount + bufferOffsetFrames,
        playbackRatePercent, due);
    if (!due) break;
    const auto offset = absolute > bufferOffsetFrames ? absolute - bufferOffsetFrames : 0;
    if (offset > cursor && !advance(offset - cursor)) return false;
    cursor = offset;
    if (!event.setVoiceGain && !admit(admit, event, event.soundData, frameCount - cursor))
      return false;
  }
  return advance(frameCount - cursor);
}

static std::uint32_t ScheduledOffset(const AudioCallbackState &state, bool &due) {
  if (state.scheduledSoundCount == 0) { due = false; return 0; }
  const auto absolute = outputOffsetForStartMicros(state.scheduledSounds[0].startMicros,
      state.scheduledMixOriginFrame, state.scheduledMixSampleRate,
      state.scheduledMixFrames + state.scheduledMixBufferOffset,
      state.scheduledMixRatePercent, due);
  return absolute > state.scheduledMixBufferOffset
      ? absolute - state.scheduledMixBufferOffset : 0;
}

static void ApplyNextScheduledSound(AudioCallbackState &state) {
  const auto event = state.scheduledSounds[0];
  if (event.setVoiceGain) SetVoiceGain(state, event.voiceId, event.gain);
  else AppendActiveSound(state, event.soundData, event.bus, 0, event.startFrame,
                         event.gain, event.loop, event.voiceId, event.startFractionQ32);
  state.scheduledSounds.consume(1);
  --state.scheduledSoundCount;
  if (event.bus != Bus::System)
    state.scheduledNonSystemSounds.fetch_sub(1, std::memory_order_release);
}

void ActivateScheduledSounds(AudioCallbackState &state,
                             long long bufferStartMicros, int sampleRate,
                             std::uint32_t frameCount,
                             int playbackRatePercent,
                             std::uint32_t bufferOffsetFrames,
                             std::optional<std::int64_t> bufferStartFrame) {
  state.scheduledMixPending = false;
  if (frameCount > std::numeric_limits<std::uint32_t>::max() - bufferOffsetFrames) return;
  state.scheduledMixOriginFrame = bufferStartFrame.value_or(
      frame_timing::at(bufferStartMicros, sampleRate, playbackRatePercent));
  state.scheduledMixSampleRate = sampleRate;
  state.scheduledMixRatePercent = playbackRatePercent;
  state.scheduledMixFrames = frameCount;
  state.scheduledMixBufferOffset = bufferOffsetFrames;
  state.scheduledMixPending = true;
  // Starts at this boundary are immediately active. Later starts remain in the
  // bounded ring until mixing has finished their predecessors' exact prefix.
  for (;;) {
    bool due = false;
    const auto offset = ScheduledOffset(state, due);
    if (!due) { state.scheduledMixPending = false; break; }
    if (offset != 0) break;
    ApplyNextScheduledSound(state);
  }
}

static void MixActiveSpan(AudioCallbackState &state, std::span<float> mixBuffer,
                     std::uint32_t frameCount, int outputChannels,
                     float bgmGain, float keysoundGain,
                     int playbackRatePercent, MixScope scope) {
  constexpr float kMixHeadroom = 0.9f;
  if (outputChannels <= 0 ||
      mixBuffer.size() < static_cast<size_t>(frameCount) * outputChannels) {
    return;
  }

  const std::uint64_t rateIncrement = rateIncrementQ32(playbackRatePercent);
  size_t soundIndex = 0;
  while (soundIndex < state.playingSoundCount) {
    PlayingSound &playingSound = state.playingSounds[soundIndex];
    SoundData *soundData = playingSound.soundData;
    const auto source = outputPcmView(soundData);
    const size_t sourceFrame =
        static_cast<size_t>(playingSound.sourceFrameQ32 >> 32);
    if (source.pending) return;
    if (soundData == nullptr || source.channels <= 0 ||
        sourceFrame >= source.frameCount) {
      removeActiveSoundAt(state, soundIndex);
      continue;
    }

    // When only system sounds are allowed (the gameplay clock is stopped and
    // BGM/keysounds must not resume), leave the non-system voices active but do
    // not advance or mix them.
    if (scope == MixScope::SystemOnly && playingSound.bus != Bus::System) {
      ++soundIndex;
      continue;
    }

    const std::uint32_t outputOffsetFrames =
        std::min(playingSound.outputOffsetFrames, frameCount);
    const auto outputEndOffsetFrames = std::min(playingSound.outputEndOffsetFrames, frameCount);
    const int channels = source.channels;
    const float busGain = playingSound.bus == Bus::Bgm
                              ? bgmGain
                              : playingSound.bus == Bus::Keysound
                                    ? keysoundGain
                                    : 1.0F;
    const float gain = kMixHeadroom * busGain * playingSound.gain;
    const std::uint64_t loopLengthQ32 =
        frameToQ32(source.frameCount);

    std::uint64_t positionQ32 = playingSound.sourceFrameQ32;
    bool finished = false;
    // Select PCM storage once per voice; contiguous gameplay audio must not
    // pay page-publication branches for every interpolated sample.
    const auto mixFrames = [&]<bool Paged>() {
      size_t cachedPage = std::numeric_limits<size_t>::max();
      const short *cachedSamples = nullptr;
      const auto frameSamples = [&](size_t frame) {
        if constexpr (!Paged) return source.frameSamples<false>(frame);
        else {
          // Published pages are immutable and stay alive until callback
          // quiescence. Acquire once per page transition, not twice per sample.
          const size_t page = frame / PagedPcmSound::kPageFrames;
          if (page != cachedPage) {
            cachedSamples = source.paged->pageSamples(page);
            cachedPage = page;
          }
          return cachedSamples == nullptr ? nullptr
              : cachedSamples + (frame % PagedPcmSound::kPageFrames) * channels;
        }
      };
      const auto audibleFrames = outputEndOffsetFrames > outputOffsetFrames
                                     ? outputEndOffsetFrames - outputOffsetFrames : 0;
      for (size_t frame = 0; frame < audibleFrames; ++frame) {
        if (playingSound.loop && positionQ32 >= loopLengthQ32) {
          positionQ32 %= loopLengthQ32;
        }
        const size_t leftFrame = static_cast<size_t>(positionQ32 >> 32);
        if (leftFrame >= source.frameCount) {
          finished = true;
          break;
        }
        const size_t rightFrame = leftFrame + 1 < source.frameCount
                                      ? leftFrame + 1
                                      : leftFrame;
        const float fraction =
            static_cast<float>(positionQ32 & kQ32FractionMask) /
            static_cast<float>(kQ32One);
        const short *leftSamples = frameSamples(leftFrame);
        const short *rightSamples = frameSamples(rightFrame);
        // Paged callers preflight the whole block before activation. Guard direct
        // misuse as well; never dereference unpublished PCM.
        if constexpr (Paged) {
          if (leftSamples == nullptr || rightSamples == nullptr) return false;
        }
        const size_t outputFrameOffset =
            (outputOffsetFrames + frame) * static_cast<size_t>(outputChannels);

        if (channels == 1) {
          const float left = static_cast<float>(leftSamples[0]);
          const float right = static_cast<float>(rightSamples[0]);
          const float sample = (left + (right - left) * fraction) / 32768.0f;
          for (int outputChannel = 0; outputChannel < outputChannels;
               ++outputChannel) {
            mixBuffer[outputFrameOffset + outputChannel] += sample * gain;
          }
        } else {
          for (int channel = 0; channel < channels; ++channel) {
            const int outputChannel = channel % outputChannels;
            const float left =
                static_cast<float>(leftSamples[channel]);
            const float right =
                static_cast<float>(rightSamples[channel]);
            const float sample = (left + (right - left) * fraction) / 32768.0f;
            mixBuffer[outputFrameOffset + outputChannel] += sample * gain;
          }
        }

        if (positionQ32 >
            std::numeric_limits<std::uint64_t>::max() - rateIncrement) {
          positionQ32 = std::numeric_limits<std::uint64_t>::max();
          finished = true;
          break;
        }
        positionQ32 += rateIncrement;
        if (static_cast<size_t>(positionQ32 >> 32) >=
            source.frameCount) {
          if (playingSound.loop) {
            positionQ32 %= loopLengthQ32;
          } else {
            finished = true;
            break;
          }
        }
      }
      return true;
    };
    const bool mixed = source.paged != nullptr
        ? mixFrames.template operator()<true>()
        : mixFrames.template operator()<false>();
    if (!mixed) return;

    playingSound.sourceFrameQ32 = positionQ32;
    playingSound.outputOffsetFrames -= outputOffsetFrames;
    if (playingSound.outputEndOffsetFrames <= frameCount) finished = true;
    else if (playingSound.outputEndOffsetFrames != std::numeric_limits<std::uint32_t>::max())
      playingSound.outputEndOffsetFrames -= frameCount;
    if (finished) {
      removeActiveSoundAt(state, soundIndex);
      continue;
    }
    ++soundIndex;
  }
}

void MixActiveSounds(AudioCallbackState &state, std::span<float> mixBuffer,
                     std::uint32_t frameCount, int outputChannels,
                     float bgmGain, float keysoundGain,
                     int playbackRatePercent, MixScope scope) {
  if (outputChannels <= 0 || mixBuffer.size() < static_cast<size_t>(frameCount) * outputChannels)
    return;
  const bool scheduled = state.scheduledMixPending && scope == MixScope::AllBuses;
  state.scheduledMixPending = false;
  std::uint32_t cursor = 0;
  while (cursor < frameCount) {
    bool due = false;
    const auto offset = scheduled ? ScheduledOffset(state, due) : frameCount;
    const auto boundary = due ? std::min(offset, frameCount) : frameCount;
    if (boundary > cursor) {
      MixActiveSpan(state, mixBuffer.subspan(static_cast<size_t>(cursor) * outputChannels),
                    boundary - cursor, outputChannels, bgmGain, keysoundGain,
                    playbackRatePercent, scope);
      cursor = boundary;
    }
    if (!due || offset >= frameCount) break;
    ApplyNextScheduledSound(state);
  }
}

} // namespace playback

} // namespace audio
