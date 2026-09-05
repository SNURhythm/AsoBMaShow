#include "AudioMix.h"
#include "../settings/AudioVideoSettings.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

AudioCallbackState::AudioCallbackState()
    : playingSounds(std::make_unique<PlayingSound[]>(kMaxActiveSounds)),
      scheduledSounds(std::make_unique<ScheduledSound[]>(kMaxScheduledSounds)),
      commandQueue(
          std::make_unique<AudioCommand[]>(kCombinedAudioCommandQueueSize)),
      realtimeCommandQueue(
          std::make_unique<AudioCommand[]>(kRealtimeAudioCommandQueueSize)) {}

namespace audio {
namespace {

float ClampVolume(float value) {
  if (!std::isfinite(value)) {
    return 1.0f;
  }
  return std::clamp(value, 0.0f, 1.0f);
}

std::uint32_t outputOffsetForStartMicros(long long startMicros,
                                         long long bufferStartMicros,
                                         int sampleRate,
                                         std::uint32_t frameCount,
                                         int playbackRatePercent, bool &isDue) {
  isDue = true;
  if (startMicros <= bufferStartMicros) {
    return 0;
  }

  const int ratePercent = playbackRatePercent > 0 ? playbackRatePercent : 100;
#if defined(__SIZEOF_INT128__)
  const __int128 deltaMicros = static_cast<__int128>(startMicros) -
                               static_cast<__int128>(bufferStartMicros);
  const __int128 denominator = static_cast<__int128>(1000000) * ratePercent;
  const __int128 roundedFrame =
      (deltaMicros * sampleRate * 100 + denominator / 2) / denominator;
#else
  const long double deltaMicros = static_cast<long double>(startMicros) -
                                  static_cast<long double>(bufferStartMicros);
  const long double roundedFrame = std::floor(
      deltaMicros * sampleRate * 100.0L / (1000000.0L * ratePercent) + 0.5L);
#endif
  if (roundedFrame >= frameCount) {
    isDue = false;
    return 0;
  }
  return static_cast<std::uint32_t>(roundedFrame);
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
  const int ratePercent = playbackRatePercent > 0 ? playbackRatePercent : 100;
  return (static_cast<std::uint64_t>(ratePercent) * kQ32One + 50ULL) / 100ULL;
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
  --state.playingSoundCount;
  if (index < state.playingSoundCount) {
    state.playingSounds[index] = state.playingSounds[state.playingSoundCount];
  }
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
  if (sourceRate == targetRate) return sourceSamples;

  const size_t sourceFrames = sourceSamples / channelCount;
  if (sourceFrames == 0) return std::size_t{0};

  const size_t sourceRateValue = static_cast<size_t>(sourceRate);
  const size_t targetRateValue = static_cast<size_t>(targetRate);
  const size_t maximumFrames =
      std::numeric_limits<size_t>::max() / channelCount;
  const size_t wholeSeconds = sourceFrames / sourceRateValue;
  const size_t remainingFrames = sourceFrames % sourceRateValue;
  if (wholeSeconds > maximumFrames / targetRateValue) {
    return std::nullopt;
  }
  const size_t wholeTargetFrames = wholeSeconds * targetRateValue;
  const std::uint64_t fractionalNumerator =
      static_cast<std::uint64_t>(remainingFrames) *
      static_cast<std::uint64_t>(targetRate);
  const size_t fractionalTargetFrames = static_cast<size_t>(
      fractionalNumerator / static_cast<std::uint64_t>(sourceRate) +
      (fractionalNumerator % static_cast<std::uint64_t>(sourceRate) != 0));
  if (fractionalTargetFrames > maximumFrames - wholeTargetFrames) {
    return std::nullopt;
  }
  const size_t targetFrames = wholeTargetFrames + fractionalTargetFrames;
  return targetFrames * channelCount;
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
                               int sourceRate, int targetRate) {
  const auto projectedSamples = ProjectedResampledPcmSampleCount(
      source.size(), channels, sourceRate, targetRate);
  if (!projectedSamples || source.empty()) return {};
  if (sourceRate == targetRate) return {source.begin(), source.end()};

  const size_t channelCount = static_cast<size_t>(channels);
  const size_t sourceFrames = source.size() / channelCount;
  const size_t targetFrames = *projectedSamples / channelCount;
  std::vector<short> output(*projectedSamples);

  for (size_t targetFrame = 0; targetFrame < targetFrames; ++targetFrame) {
    const long double sourcePosition =
        static_cast<long double>(targetFrame) * sourceRate / targetRate;
    const size_t leftFrame =
        std::min(static_cast<size_t>(sourcePosition), sourceFrames - 1);
    const size_t rightFrame = std::min(leftFrame + 1, sourceFrames - 1);
    const long double fraction = sourcePosition - leftFrame;

    for (int channel = 0; channel < channels; ++channel) {
      const size_t leftIndex = leftFrame * channelCount + channel;
      const size_t rightIndex = rightFrame * channelCount + channel;
      const long double interpolated =
          static_cast<long double>(source[leftIndex]) * (1.0L - fraction) +
          static_cast<long double>(source[rightIndex]) * fraction;
      const long rounded = std::lround(interpolated);
      output[targetFrame * channelCount + channel] =
          static_cast<short>(std::clamp(
              rounded, static_cast<long>(std::numeric_limits<short>::min()),
              static_cast<long>(std::numeric_limits<short>::max())));
    }
  }

  return output;
}

namespace playback {
namespace {

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

} // namespace

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
  for (SoundData *soundData : sounds) {
    if (soundData == nullptr) {
      continue;
    }
    if (soundData->channels <= 0 || soundData->sourceSampleRate <= 0 ||
        soundData->sourceData.size() %
                static_cast<size_t>(soundData->channels) !=
            0) {
      return std::nullopt;
    }

    auto outputData =
        ResamplePcm(soundData->sourceData, soundData->channels,
                    soundData->sourceSampleRate, targetSampleRate);
    if (!soundData->sourceData.empty() && outputData.empty()) {
      return std::nullopt;
    }
    const size_t outputFrameCount =
        outputData.size() / static_cast<size_t>(soundData->channels);
    transition.candidates.push_back({.soundData = soundData,
                                     .outputData = std::move(outputData),
                                     .outputFrameCount = outputFrameCount});
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
  }

  for (size_t index = 0; index < state.scheduledSoundCount; ++index) {
    auto &scheduledSound = state.scheduledSounds[index];
    scheduledSound.startFrame =
        clampFrameToSound(RemapFramePosition(scheduledSound.startFrame,
                                             transition.previousSampleRate,
                                             transition.targetSampleRate),
                          scheduledSound.soundData);
  }

  const std::uint32_t readCursor =
      state.commandReadCursor.load(std::memory_order_acquire);
  const std::uint32_t writeCursor =
      state.commandWriteCursor.load(std::memory_order_acquire);
  for (std::uint32_t cursor = readCursor; cursor != writeCursor; ++cursor) {
    auto &command = state.commandQueue[cursor % kCombinedAudioCommandQueueSize];
    command.startFrame = clampFrameToSound(
        RemapFramePosition(command.startFrame, transition.previousSampleRate,
                           transition.targetSampleRate),
        command.soundData);
  }

  const std::uint32_t realtimeReadCursor =
      state.realtimeCommandReadCursor.load(std::memory_order_acquire);
  const std::uint32_t realtimeWriteCursor =
      state.realtimeCommandWriteCursor.load(std::memory_order_acquire);
  for (std::uint32_t cursor = realtimeReadCursor;
       cursor != realtimeWriteCursor; ++cursor) {
    auto &command = state.realtimeCommandQueue[
        cursor % kRealtimeAudioCommandQueueSize];
    command.startFrame = clampFrameToSound(
        RemapFramePosition(command.startFrame, transition.previousSampleRate,
                           transition.targetSampleRate),
        command.soundData);
  }
}

bool AppendActiveSound(AudioCallbackState &state, SoundData *soundData, Bus bus,
                       std::uint32_t outputOffsetFrames, size_t startFrame,
                       float gain, bool loop) {
  if (soundData == nullptr || startFrame >= soundData->outputFrameCount ||
      startFrame > kQ32MaximumWholeFrame ||
      state.playingSoundCount >= kMaxActiveSounds) {
    return false;
  }
  soundData->playing = true;
  state.playingSounds[state.playingSoundCount++] = {
      .soundData = soundData,
      .bus = bus,
      .sourceFrameQ32 = frameToQ32(startFrame),
      .outputOffsetFrames = outputOffsetFrames,
      .gain = gain,
      .loop = loop,
  };
  return true;
}

static bool AppendRealtimeActiveSound(AudioCallbackState &state,
                                      SoundData *soundData, Bus bus,
                                      size_t startFrame) {
  if (soundData == nullptr || startFrame >= soundData->outputFrameCount ||
      startFrame > kQ32MaximumWholeFrame) {
    return false;
  }
  if (state.playingSoundCount >= kMaxActiveSounds) {
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
  return AppendActiveSound(state, soundData, bus, 0, startFrame);
}

bool InsertScheduledSound(AudioCallbackState &state,
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
  return true;
}

void ClearCallbackSounds(AudioCallbackState &state) {
  for (size_t index = 0; index < state.playingSoundCount; ++index) {
    if (state.playingSounds[index].soundData) {
      state.playingSounds[index].soundData->playing = false;
    }
  }
  state.playingSoundCount = 0;
  state.scheduledSoundCount = 0;
}

void RemoveSound(AudioCallbackState &state, SoundData *soundData) {
  std::size_t active = 0;
  while (active < state.playingSoundCount) {
    if (state.playingSounds[active].soundData == soundData) {
      removeActiveSoundAt(state, active);
    } else {
      ++active;
    }
  }
  std::size_t retained = 0;
  for (std::size_t index = 0; index < state.scheduledSoundCount; ++index) {
    if (state.scheduledSounds[index].soundData != soundData) {
      state.scheduledSounds[retained++] = state.scheduledSounds[index];
    }
  }
  state.scheduledSoundCount = retained;
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
  state.realtimeCommandWriteCursor.store(writeCursor + 1,
                                         std::memory_order_release);
  return true;
}

void DrainRealtimeCommands(AudioCallbackState &state) noexcept {
  std::uint32_t readCursor =
      state.realtimeCommandReadCursor.load(std::memory_order_relaxed);
  const std::uint32_t writeCursor =
      state.realtimeCommandWriteCursor.load(std::memory_order_acquire);
  while (readCursor != writeCursor) {
    const AudioCommand &command = state.realtimeCommandQueue[
        readCursor % kRealtimeAudioCommandQueueSize];
    switch (command.type) {
    case AudioCommandType::PlayNow:
      AppendRealtimeActiveSound(state, command.soundData, command.bus,
                                command.startFrame);
      break;
    case AudioCommandType::Schedule:
      InsertScheduledSound(state, {.soundData = command.soundData,
                                   .bus = command.bus,
                                   .startMicros = command.startMicros,
                                   .sequence = command.sequence,
                                   .startFrame = command.startFrame,
                                   .gain = command.gain,
                                   .loop = command.loop});
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
                      command.startFrame, command.gain, command.loop);
    break;
  case AudioCommandType::Schedule:
    InsertScheduledSound(state, {.soundData = command.soundData,
                                 .bus = command.bus,
                                 .startMicros = command.startMicros,
                                 .sequence = command.sequence,
                                 .startFrame = command.startFrame,
                                 .gain = command.gain,
                                 .loop = command.loop});
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

void ActivateScheduledSounds(AudioCallbackState &state,
                             long long bufferStartMicros, int sampleRate,
                             std::uint32_t frameCount,
                             int playbackRatePercent) {
  size_t scheduledSoundsToRemove = 0;
  for (; scheduledSoundsToRemove < state.scheduledSoundCount;
       ++scheduledSoundsToRemove) {
    const ScheduledSound &scheduledSound =
        state.scheduledSounds[scheduledSoundsToRemove];
    bool isDue = false;
    const std::uint32_t outputOffsetFrames = outputOffsetForStartMicros(
        scheduledSound.startMicros, bufferStartMicros, sampleRate, frameCount,
        playbackRatePercent, isDue);
    if (!isDue) {
      break;
    }
    AppendActiveSound(state, scheduledSound.soundData, scheduledSound.bus,
                      outputOffsetFrames, scheduledSound.startFrame,
                      scheduledSound.gain, scheduledSound.loop);
  }

  if (scheduledSoundsToRemove == 0) {
    return;
  }
  const size_t remainingSounds =
      state.scheduledSoundCount - scheduledSoundsToRemove;
  for (size_t index = 0; index < remainingSounds; ++index) {
    state.scheduledSounds[index] =
        state.scheduledSounds[index + scheduledSoundsToRemove];
  }
  state.scheduledSoundCount = remainingSounds;
}

void MixActiveSounds(AudioCallbackState &state, std::span<float> mixBuffer,
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
    const size_t sourceFrame =
        static_cast<size_t>(playingSound.sourceFrameQ32 >> 32);
    if (soundData == nullptr || soundData->channels <= 0 ||
        sourceFrame >= soundData->outputFrameCount) {
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
    const short *source = soundData->outputData.data();
    const int channels = soundData->channels;
    const float busGain = playingSound.bus == Bus::Bgm
                              ? bgmGain
                              : playingSound.bus == Bus::Keysound
                                    ? keysoundGain
                                    : 1.0F;
    const float gain = kMixHeadroom * busGain * playingSound.gain;
    const std::uint64_t loopLengthQ32 =
        frameToQ32(soundData->outputFrameCount);

    std::uint64_t positionQ32 = playingSound.sourceFrameQ32;
    bool finished = false;
    for (size_t frame = 0; frame < frameCount - outputOffsetFrames; ++frame) {
      if (playingSound.loop && positionQ32 >= loopLengthQ32) {
        positionQ32 %= loopLengthQ32;
      }
      const size_t leftFrame = static_cast<size_t>(positionQ32 >> 32);
      if (leftFrame >= soundData->outputFrameCount) {
        finished = true;
        break;
      }
      const size_t rightFrame = leftFrame + 1 < soundData->outputFrameCount
                                    ? leftFrame + 1
                                    : leftFrame;
      const float fraction =
          static_cast<float>(positionQ32 & kQ32FractionMask) /
          static_cast<float>(kQ32One);
      const size_t leftFrameOffset = leftFrame * static_cast<size_t>(channels);
      const size_t rightFrameOffset =
          rightFrame * static_cast<size_t>(channels);
      const size_t outputFrameOffset =
          (outputOffsetFrames + frame) * static_cast<size_t>(outputChannels);

      if (channels == 1) {
        const float left = static_cast<float>(source[leftFrameOffset]);
        const float right = static_cast<float>(source[rightFrameOffset]);
        const float sample = (left + (right - left) * fraction) / 32768.0f;
        for (int outputChannel = 0; outputChannel < outputChannels;
             ++outputChannel) {
          mixBuffer[outputFrameOffset + outputChannel] += sample * gain;
        }
      } else {
        for (int channel = 0; channel < channels; ++channel) {
          const int outputChannel = channel % outputChannels;
          const float left =
              static_cast<float>(source[leftFrameOffset + channel]);
          const float right =
              static_cast<float>(source[rightFrameOffset + channel]);
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
          soundData->outputFrameCount) {
        if (playingSound.loop) {
          positionQ32 %= loopLengthQ32;
        } else {
          finished = true;
          break;
        }
      }
    }

    playingSound.sourceFrameQ32 = positionQ32;
    playingSound.outputOffsetFrames = 0;
    if (finished) {
      removeActiveSoundAt(state, soundIndex);
      continue;
    }
    ++soundIndex;
  }
}

} // namespace playback

} // namespace audio
