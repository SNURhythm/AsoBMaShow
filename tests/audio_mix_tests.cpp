#include "audio/AudioMix.h"
#include "audio/AudioWrapper.h"
#include "audio/ChartAudioRenderer.h"
#include "audio/Jukebox.h"

#include <array>
#include <cmath>
#include <concepts>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

class FakeBackendLifecycle final : public audio::playback::IBackendLifecycle {
public:
  std::vector<audio::playback::BackendStateObservation> observations;
  audio::playback::BackendOperationResult stopResult{.success = true};
  audio::playback::BackendOperationResult startResult{.success = true};
  mutable size_t observationIndex = 0;
  mutable int observeCalls = 0;
  int stopCalls = 0;
  int startCalls = 0;
  int sampleRate = 48000;
  std::function<void()> onStopAndDrain;
  std::function<void()> onStart;

  audio::playback::BackendStateObservation observeState() const override {
    ++observeCalls;
    if (observations.empty()) {
      return {.state = audio::playback::BackendRunState::Unknown,
              .diagnostic = "fake has no state observation"};
    }
    const size_t index = std::min(observationIndex, observations.size() - 1);
    ++observationIndex;
    return observations[index];
  }

  int outputSampleRate() const override { return sampleRate; }

  audio::playback::BackendOperationResult stopAndDrain() override {
    ++stopCalls;
    if (onStopAndDrain) {
      onStopAndDrain();
    }
    return stopResult;
  }

  audio::playback::BackendOperationResult start() override {
    ++startCalls;
    if (onStart) {
      onStart();
    }
    return startResult;
  }
};

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

using PlaySoundSignature = bool (AudioWrapper::*)(const path_t &, audio::Bus,
                                                  long long);
using ScheduleSoundSignature = bool (AudioWrapper::*)(const path_t &,
                                                      audio::Bus, long long);
using SetVolumesSignature = void (AudioWrapper::*)(const audio::Volumes &);
using SetSettingsVolumesSignature =
    void (AudioWrapper::*)(const player_settings::AudioSettings &);
using StartDeviceSignature =
    audio::playback::BackendOperationResult (AudioWrapper::*)();
using StopSoundsSignature =
    audio::playback::BackendOperationResult (AudioWrapper::*)();
using SetPlaybackRateSignature = bool (AudioWrapper::*)(audio::PlaybackRate,
                                                        std::string &);

static_assert(std::same_as<decltype(std::declval<SoundData>().sourceData),
                           std::vector<short>>);
static_assert(std::same_as<decltype(std::declval<SoundData>().outputData),
                           std::vector<short>>);
static_assert(
    std::same_as<decltype(std::declval<SoundData>().sourceFrameCount), size_t>);
static_assert(
    std::same_as<decltype(std::declval<SoundData>().outputFrameCount), size_t>);
static_assert(
    std::same_as<decltype(std::declval<SoundData>().sourceSampleRate), int>);
static_assert(
    std::same_as<decltype(std::declval<PlayingSound>().bus), audio::Bus>);
static_assert(
    std::same_as<decltype(std::declval<PlayingSound>().sourceFrameQ32),
                 std::uint64_t>);
static_assert(
    std::same_as<decltype(std::declval<ScheduledSound>().bus), audio::Bus>);
static_assert(
    std::same_as<decltype(std::declval<AudioCommand>().bus), audio::Bus>);
static_assert(std::same_as<decltype(static_cast<PlaySoundSignature>(
                               &AudioWrapper::playSound)),
                           PlaySoundSignature>);
static_assert(std::same_as<decltype(static_cast<ScheduleSoundSignature>(
                               &AudioWrapper::scheduleSound)),
                           ScheduleSoundSignature>);
static_assert(std::same_as<decltype(static_cast<SetVolumesSignature>(
                               &AudioWrapper::setVolumes)),
                           SetVolumesSignature>);
static_assert(std::same_as<decltype(static_cast<SetSettingsVolumesSignature>(
                               &AudioWrapper::setVolumes)),
                           SetSettingsVolumesSignature>);
static_assert(std::same_as<decltype(static_cast<StartDeviceSignature>(
                               &AudioWrapper::startDevice)),
                           StartDeviceSignature>);
static_assert(std::same_as<decltype(static_cast<StopSoundsSignature>(
                               &AudioWrapper::stopSounds)),
                           StopSoundsSignature>);
static_assert(std::same_as<decltype(static_cast<SetPlaybackRateSignature>(
                               &AudioWrapper::setPlaybackRate)),
                           SetPlaybackRateSignature>);

std::vector<short> stereoRamp(size_t frameCount) {
  std::vector<short> result;
  result.reserve(frameCount * 2);
  for (size_t frame = 0; frame < frameCount; ++frame) {
    result.push_back(static_cast<short>(frame % 30000));
    result.push_back(static_cast<short>(-static_cast<int>(frame % 30000)));
  }
  return result;
}

void requireNear(float actual, float expected, std::string_view message) {
  require(std::fabs(actual - expected) < 0.0001f, message);
}

void testStoppedQueryInterpretationPreservesErrors() {
  const auto stopped =
      audio::playback::InterpretStoppedQueryResult(1, "unused");
  const auto running =
      audio::playback::InterpretStoppedQueryResult(0, "unused");
  const auto failed = audio::playback::InterpretStoppedQueryResult(
      -9988, "PortAudio state query failed");

  require(stopped.state == audio::playback::BackendRunState::Stopped &&
              running.state == audio::playback::BackendRunState::Running,
          "binary native states map to stopped and running");
  require(failed.state == audio::playback::BackendRunState::Unknown &&
              failed.diagnostic == "PortAudio state query failed",
          "a negative native state-query error remains explicit and unknown");
}

void testUnknownBackendStateCannotPublishRateTransition() {
  SoundData sound;
  sound.channels = 1;
  sound.sourceSampleRate = 44100;
  sound.sourceData = {100, 200, 300, 400};
  sound.sourceFrameCount = sound.sourceData.size();
  sound.outputData = sound.sourceData;
  sound.outputFrameCount = sound.outputData.size();

  AudioCallbackState callbackState;
  std::atomic<int> sampleRate{44100};
  std::atomic<int64_t> audioClockFrameCursor{441};
  std::atomic<audio::playback::BackendRunState> backendState{
      audio::playback::BackendRunState::Running};
  FakeBackendLifecycle backend;
  backend.observations = {
      {.state = audio::playback::BackendRunState::Unknown,
       .diagnostic = "PortAudio state query failed"},
  };
  std::array<SoundData *, 1> sounds{&sound};

  const auto result = audio::playback::EnsureBackendStartedAtOutputRate(
      backend, sounds, callbackState, 48000, sampleRate, audioClockFrameCursor,
      backendState);

  require(!result.success &&
              result.diagnostic.find("PortAudio state query failed") !=
                  std::string::npos,
          "an unknown backend state fails with the native diagnostic");
  require(sound.outputData == sound.sourceData &&
              sound.outputFrameCount == sound.sourceFrameCount &&
              sampleRate.load() == 44100 && audioClockFrameCursor.load() == 441,
          "an unknown backend state publishes no PCM, rate, or clock changes");
  require(
      backendState.load() != audio::playback::BackendRunState::Stopped &&
          !audio::playback::CanMutateCallbackStateDirectly(backendState.load()),
      "unknown callback quiescence is never published as directly mutable");
  require(backend.stopCalls == 0 && backend.startCalls == 0,
          "an unknown backend state performs no lifecycle operation");
}

void testStopDrainFailureCannotPublishRateTransition() {
  SoundData sound;
  sound.channels = 1;
  sound.sourceSampleRate = 44100;
  sound.sourceData = {100, 200, 300, 400};
  sound.sourceFrameCount = sound.sourceData.size();
  sound.outputData = sound.sourceData;
  sound.outputFrameCount = sound.outputData.size();

  AudioCallbackState callbackState;
  std::atomic<int> sampleRate{44100};
  std::atomic<int64_t> audioClockFrameCursor{441};
  std::atomic<audio::playback::BackendRunState> backendState{
      audio::playback::BackendRunState::Running};
  FakeBackendLifecycle backend;
  backend.observations = {
      {.state = audio::playback::BackendRunState::Running},
  };
  backend.stopResult = {.success = false,
                        .diagnostic = "PortAudio stop timed out"};
  std::array<SoundData *, 1> sounds{&sound};

  const auto result = audio::playback::EnsureBackendStartedAtOutputRate(
      backend, sounds, callbackState, 48000, sampleRate, audioClockFrameCursor,
      backendState);

  require(!result.success &&
              result.diagnostic.find("PortAudio stop timed out") !=
                  std::string::npos,
          "a stop-and-drain failure propagates the native diagnostic");
  require(sound.outputData == sound.sourceData &&
              sound.outputFrameCount == sound.sourceFrameCount &&
              sampleRate.load() == 44100 && audioClockFrameCursor.load() == 441,
          "a failed drain publishes no PCM, rate, or clock changes");
  require(
      backendState.load() != audio::playback::BackendRunState::Stopped &&
          !audio::playback::CanMutateCallbackStateDirectly(backendState.load()),
      "a failed drain never makes callback state directly mutable");
  require(backend.stopCalls == 1 && backend.startCalls == 0,
          "a failed drain is attempted once and never followed by start");
}

void testPostStopStateErrorCannotPublishRateTransition() {
  SoundData sound;
  sound.channels = 1;
  sound.sourceSampleRate = 44100;
  sound.sourceData = {100, 200, 300, 400};
  sound.sourceFrameCount = sound.sourceData.size();
  sound.outputData = sound.sourceData;
  sound.outputFrameCount = sound.outputData.size();

  AudioCallbackState callbackState;
  std::atomic<int> sampleRate{44100};
  std::atomic<int64_t> audioClockFrameCursor{441};
  std::atomic<audio::playback::BackendRunState> backendState{
      audio::playback::BackendRunState::Running};
  FakeBackendLifecycle backend;
  backend.observations = {
      {.state = audio::playback::BackendRunState::Running},
      {.state = audio::playback::BackendRunState::Unknown,
       .diagnostic = "post-stop state unavailable"},
  };
  std::array<SoundData *, 1> sounds{&sound};

  const auto result = audio::playback::EnsureBackendStartedAtOutputRate(
      backend, sounds, callbackState, 48000, sampleRate, audioClockFrameCursor,
      backendState);

  require(!result.success &&
              result.diagnostic.find("post-stop state unavailable") !=
                  std::string::npos,
          "rate publication requires positive stopped-state confirmation");
  require(sound.outputData == sound.sourceData &&
              sound.outputFrameCount == sound.sourceFrameCount &&
              sampleRate.load() == 44100 && audioClockFrameCursor.load() == 441,
          "an unconfirmed successful stop still publishes no transition");
  require(
      backendState.load() == audio::playback::BackendRunState::Unknown &&
          !audio::playback::CanMutateCallbackStateDirectly(backendState.load()),
      "a post-stop state error remains fail-closed");
  require(backend.observeCalls == 2 && backend.stopCalls == 1 &&
              backend.startCalls == 0,
          "the backend is re-observed after draining and never restarted on "
          "uncertainty");
}

void testConfirmedDrainPublishesThenRestartsAtNewRate() {
  SoundData sound;
  sound.channels = 1;
  sound.sourceSampleRate = 44100;
  sound.sourceData.resize(441);
  for (size_t frame = 0; frame < sound.sourceData.size(); ++frame) {
    sound.sourceData[frame] = static_cast<short>(frame);
  }
  sound.sourceFrameCount = sound.sourceData.size();
  sound.outputData = sound.sourceData;
  sound.outputFrameCount = sound.outputData.size();

  AudioCallbackState callbackState;
  callbackState.playingSounds[0] = {.soundData = &sound,
                                    .sourceFrameQ32 = 220ULL << 32};
  callbackState.playingSoundCount = 1;
  std::atomic<int> sampleRate{44100};
  std::atomic<int64_t> audioClockFrameCursor{441};
  std::atomic<audio::playback::BackendRunState> backendState{
      audio::playback::BackendRunState::Running};
  FakeBackendLifecycle backend;
  backend.observations = {
      {.state = audio::playback::BackendRunState::Running},
      {.state = audio::playback::BackendRunState::Stopped},
      {.state = audio::playback::BackendRunState::Running},
  };
  backend.onStopAndDrain = [&] {
    require(sound.outputFrameCount == 441 && sampleRate.load() == 44100,
            "draining happens before any callback-visible publication");
  };
  backend.onStart = [&] {
    require(
        sound.outputFrameCount == 480 && sampleRate.load() == 48000 &&
            (callbackState.playingSounds[0].sourceFrameQ32 >> 32) == 239 &&
            audioClockFrameCursor.load() == 480,
        "restart happens only after PCM, positions, clock, and rate publish");
  };
  std::array<SoundData *, 1> sounds{&sound};

  const auto result = audio::playback::EnsureBackendStartedAtOutputRate(
      backend, sounds, callbackState, 48000, sampleRate, audioClockFrameCursor,
      backendState);

  require(result.success && result.diagnostic.empty(),
          "a confirmed drain, transition, and restart succeeds");
  require(sound.outputFrameCount == 480 && sampleRate.load() == 48000 &&
              audioClockFrameCursor.load() == 480 &&
              backendState.load() == audio::playback::BackendRunState::Running,
          "the successful lifecycle publishes a coherent running state");
  require(backend.observeCalls == 3 && backend.stopCalls == 1 &&
              backend.startCalls == 1,
          "the successful lifecycle confirms stop and start exactly once");
}

void testAlreadyRunningAtTargetRateDoesNotRestart() {
  SoundData sound;
  sound.channels = 1;
  sound.sourceSampleRate = 44100;
  sound.sourceData = {100, 200};
  sound.sourceFrameCount = sound.sourceData.size();
  sound.outputData = sound.sourceData;
  sound.outputFrameCount = sound.outputData.size();
  const auto originalOutput = sound.outputData;

  AudioCallbackState callbackState;
  std::atomic<int> sampleRate{44100};
  std::atomic<int64_t> audioClockFrameCursor{441};
  std::atomic<audio::playback::BackendRunState> backendState{
      audio::playback::BackendRunState::Unknown};
  FakeBackendLifecycle backend;
  backend.observations = {
      {.state = audio::playback::BackendRunState::Running},
  };
  std::array<SoundData *, 1> sounds{&sound};

  const auto result = audio::playback::EnsureBackendStartedAtOutputRate(
      backend, sounds, callbackState, 44100, sampleRate, audioClockFrameCursor,
      backendState);

  require(result.success &&
              backendState.load() == audio::playback::BackendRunState::Running,
          "an already-running target-rate backend remains ready");
  require(backend.stopCalls == 0 && backend.startCalls == 0 &&
              backend.observeCalls == 1,
          "an already-running target-rate backend is not restarted");
  require(sound.outputData == originalOutput && sampleRate.load() == 44100 &&
              audioClockFrameCursor.load() == 441,
          "a no-op start publishes no audio state changes");
}

void testStartFailureCannotPublishRunningState() {
  AudioCallbackState callbackState;
  std::atomic<int> sampleRate{44100};
  std::atomic<int64_t> audioClockFrameCursor{0};
  std::atomic<audio::playback::BackendRunState> backendState{
      audio::playback::BackendRunState::Stopped};
  FakeBackendLifecycle backend;
  backend.observations = {
      {.state = audio::playback::BackendRunState::Stopped},
  };
  backend.startResult = {.success = false,
                         .diagnostic = "PortAudio start was rejected"};
  const std::array<SoundData *, 0> sounds{};

  const auto result = audio::playback::EnsureBackendStartedAtOutputRate(
      backend, sounds, callbackState, 44100, sampleRate, audioClockFrameCursor,
      backendState);

  require(!result.success &&
              result.diagnostic.find("PortAudio start was rejected") !=
                  std::string::npos,
          "a native start failure is surfaced to the wrapper caller");
  require(
      backendState.load() == audio::playback::BackendRunState::Unknown &&
          !audio::playback::CanMutateCallbackStateDirectly(backendState.load()),
      "a failed start cannot publish either running or mutable state");
  require(backend.startCalls == 1 && backend.observeCalls == 1,
          "a failed start is not followed by a false running observation");
}

void testPostStartQueryFailureCannotPublishRunningState() {
  AudioCallbackState callbackState;
  std::atomic<int> sampleRate{44100};
  std::atomic<int64_t> audioClockFrameCursor{0};
  std::atomic<audio::playback::BackendRunState> backendState{
      audio::playback::BackendRunState::Stopped};
  FakeBackendLifecycle backend;
  backend.observations = {
      {.state = audio::playback::BackendRunState::Stopped},
      {.state = audio::playback::BackendRunState::Unknown,
       .diagnostic = "post-start state query failed"},
  };
  const std::array<SoundData *, 0> sounds{};

  const auto result = audio::playback::EnsureBackendStartedAtOutputRate(
      backend, sounds, callbackState, 44100, sampleRate, audioClockFrameCursor,
      backendState);

  require(!result.success &&
              result.diagnostic.find("post-start state query failed") !=
                  std::string::npos,
          "nominal start still requires a positive running observation");
  require(
      backendState.load() == audio::playback::BackendRunState::Unknown &&
          !audio::playback::CanMutateCallbackStateDirectly(backendState.load()),
      "post-start query uncertainty remains fail-closed");
  require(backend.startCalls == 1 && backend.observeCalls == 2,
          "the post-start state is observed exactly once before failure");
}

void testStopFailureCannotClearCallbackState() {
  SoundData sound;
  sound.channels = 1;
  sound.outputData = {100, 200};
  sound.outputFrameCount = sound.outputData.size();
  sound.playing = true;

  AudioCallbackState callbackState;
  callbackState.playingSounds[0] = {.soundData = &sound};
  callbackState.playingSoundCount = 1;
  callbackState.scheduledSounds[0] = {.soundData = &sound};
  callbackState.scheduledSoundCount = 1;
  callbackState.commandQueue[0] = {.type = AudioCommandType::PlayNow,
                                   .soundData = &sound};
  callbackState.commandWriteCursor.store(1);

  std::atomic<audio::playback::BackendRunState> backendState{
      audio::playback::BackendRunState::Running};
  FakeBackendLifecycle backend;
  backend.observations = {
      {.state = audio::playback::BackendRunState::Running},
  };
  backend.stopResult = {.success = false,
                        .diagnostic = "drain could not complete"};

  const auto result = audio::playback::StopBackendAndClearCallbackState(
      backend, callbackState, backendState);

  require(!result.success &&
              result.diagnostic.find("drain could not complete") !=
                  std::string::npos,
          "stop-all propagates a failed drain diagnostic");
  require(callbackState.playingSoundCount == 1 &&
              callbackState.scheduledSoundCount == 1 &&
              callbackState.commandReadCursor.load() == 0 &&
              callbackState.commandWriteCursor.load() == 1 && sound.playing,
          "a failed drain cannot mutate any callback-visible playback state");
  require(
      backendState.load() != audio::playback::BackendRunState::Stopped &&
          !audio::playback::CanMutateCallbackStateDirectly(backendState.load()),
      "failed stop-all keeps direct callback mutation disabled");
}

void testConfirmedStopClearsCallbackStateAfterDrain() {
  SoundData sound;
  sound.channels = 1;
  sound.outputData = {100, 200};
  sound.outputFrameCount = sound.outputData.size();
  sound.playing = true;

  AudioCallbackState callbackState;
  callbackState.playingSounds[0] = {.soundData = &sound};
  callbackState.playingSoundCount = 1;
  callbackState.scheduledSounds[0] = {.soundData = &sound};
  callbackState.scheduledSoundCount = 1;
  callbackState.commandQueue[0] = {.type = AudioCommandType::PlayNow,
                                   .soundData = &sound};
  callbackState.commandWriteCursor.store(1);

  std::atomic<audio::playback::BackendRunState> backendState{
      audio::playback::BackendRunState::Running};
  FakeBackendLifecycle backend;
  backend.observations = {
      {.state = audio::playback::BackendRunState::Running},
      {.state = audio::playback::BackendRunState::Stopped},
  };
  backend.onStopAndDrain = [&] {
    require(callbackState.playingSoundCount == 1 &&
                callbackState.scheduledSoundCount == 1 && sound.playing,
            "callback state remains intact until the drain completes");
  };

  const auto result = audio::playback::StopBackendAndClearCallbackState(
      backend, callbackState, backendState);

  require(result.success && result.diagnostic.empty(),
          "a positively confirmed drain permits stop-all");
  require(callbackState.playingSoundCount == 0 &&
              callbackState.scheduledSoundCount == 0 &&
              callbackState.commandReadCursor.load() == 0 &&
              callbackState.commandWriteCursor.load() == 0 && !sound.playing,
          "stop-all clears callback state only after positive quiescence");
  require(
      backendState.load() == audio::playback::BackendRunState::Stopped &&
          audio::playback::CanMutateCallbackStateDirectly(backendState.load()),
      "positive drain confirmation is the only directly mutable state");
}

void testRateTransitionRegeneratesAndRemapsEveryFrameDomain() {
  SoundData sound;
  sound.channels = 1;
  sound.sourceSampleRate = 44100;
  sound.sourceFrameCount = 88200;
  sound.sourceData.resize(sound.sourceFrameCount);
  for (size_t frame = 0; frame < sound.sourceFrameCount; ++frame) {
    sound.sourceData[frame] = static_cast<short>(frame % 30000);
  }
  sound.outputData = sound.sourceData;
  sound.outputFrameCount = sound.sourceFrameCount;
  sound.currentFrame = 4410;

  AudioCallbackState state;
  state.playingSounds[0] = {.soundData = &sound,
                            .bus = audio::Bus::Bgm,
                            .sourceFrameQ32 = 44100ULL << 32,
                            .outputOffsetFrames = 441};
  state.playingSoundCount = 1;
  state.scheduledSounds[0] = {.soundData = &sound,
                              .bus = audio::Bus::Keysound,
                              .startMicros = 2000000,
                              .sequence = 1,
                              .startFrame = 22050};
  state.scheduledSoundCount = 1;
  state.commandQueue[0] = {.type = AudioCommandType::PlayNow,
                           .soundData = &sound,
                           .bus = audio::Bus::Keysound,
                           .startFrame = 11025};
  state.commandWriteCursor.store(1);

  std::array<SoundData *, 1> sounds{&sound};
  auto transition =
      audio::playback::PrepareOutputRateTransition(sounds, 44100, 48000);
  require(transition.has_value(),
          "a valid retained PCM set prepares a new output-rate candidate");
  require(sound.outputFrameCount == 88200 &&
              state.playingSounds[0].sourceFrameQ32 == 44100ULL << 32 &&
              state.scheduledSounds[0].startFrame == 22050 &&
              state.commandQueue[0].startFrame == 11025,
          "preparing a rate transition does not publish mixed-rate state");

  audio::playback::CommitOutputRateTransition(std::move(*transition), state);
  require(sound.outputFrameCount == 96000 && sound.currentFrame == 4800,
          "committing regenerates output from retained PCM at 48 kHz");
  require(state.playingSounds[0].sourceFrameQ32 == 48000ULL << 32 &&
              state.playingSounds[0].outputOffsetFrames == 480,
          "active playback offsets retain their elapsed time at 48 kHz");
  require(state.scheduledSounds[0].startFrame == 24000,
          "scheduled offsets retain their elapsed time at 48 kHz");
  require(state.commandQueue[0].startFrame == 12000,
          "queued offsets retain their elapsed time at 48 kHz");

  auto restored =
      audio::playback::PrepareOutputRateTransition(sounds, 48000, 44100);
  require(restored.has_value(),
          "the retained source can prepare a reverse rate transition");
  audio::playback::CommitOutputRateTransition(std::move(*restored), state);
  require(
      sound.outputFrameCount == 88200 && sound.outputData == sound.sourceData,
      "returning to the source rate regenerates from source, not prior output");
  require(state.playingSounds[0].sourceFrameQ32 == 44100ULL << 32 &&
              state.playingSounds[0].outputOffsetFrames == 441 &&
              state.scheduledSounds[0].startFrame == 22050 &&
              state.commandQueue[0].startFrame == 11025,
          "all nonzero frame domains remain time-correct after 48 to 44.1 kHz");
}

std::vector<float> mixMonoRampAtRate(int playbackRatePercent,
                                     std::uint32_t outputFrames) {
  SoundData sound;
  sound.channels = 1;
  sound.outputData = {0, 1000, 2000, 3000, 4000, 5000, 6000, 7000};
  sound.outputFrameCount = sound.outputData.size();

  AudioCallbackState state;
  require(audio::playback::AppendActiveSound(state, &sound, audio::Bus::Bgm, 0),
          "mono ramp enters the active mixer");
  std::vector<float> output(outputFrames, 0.0f);
  audio::playback::MixActiveSounds(state, output, outputFrames, 1, 1.0f, 1.0f,
                                   playbackRatePercent);
  return output;
}

void testPitchShiftMixerUsesQ32Interpolation() {
  const auto fast = mixMonoRampAtRate(200, 4);
  requireNear(fast[0], 0.0f, "200 percent starts at source frame zero");
  requireNear(fast[1], 2000.0f / 32768.0f * 0.9f,
              "200 percent consumes source frame two");
  requireNear(fast[2], 4000.0f / 32768.0f * 0.9f,
              "200 percent consumes source frame four");
  requireNear(fast[3], 6000.0f / 32768.0f * 0.9f,
              "200 percent consumes source frame six");

  const auto slow = mixMonoRampAtRate(50, 4);
  requireNear(slow[0], 0.0f, "50 percent starts at source position zero");
  requireNear(slow[1], 500.0f / 32768.0f * 0.9f,
              "50 percent interpolates source position one half");
  requireNear(slow[2], 1000.0f / 32768.0f * 0.9f,
              "50 percent reaches source position one");
  requireNear(slow[3], 1500.0f / 32768.0f * 0.9f,
              "50 percent interpolates source position one and one half");
}

void testQ32ActiveCursorRejectsUnrepresentableStartFrame() {
  if constexpr (std::numeric_limits<size_t>::max() >
                std::numeric_limits<std::uint32_t>::max()) {
    SoundData sound;
    sound.channels = 1;
    sound.outputFrameCount =
        static_cast<size_t>(std::numeric_limits<std::uint32_t>::max()) + 2;
    AudioCallbackState state;
    require(
        !audio::playback::AppendActiveSound(
            state, &sound, audio::Bus::Bgm, 0,
            static_cast<size_t>(std::numeric_limits<std::uint32_t>::max()) + 1),
        "Q32 active cursors reject an unrepresentable source frame");
  }
}

void testScheduledOffsetsUseInversePlaybackRate() {
  SoundData sound;
  sound.channels = 1;
  sound.outputData = {100, 200, 300, 400};
  sound.outputFrameCount = sound.outputData.size();

  AudioCallbackState slowState;
  require(audio::playback::InsertScheduledSound(
              slowState, {.soundData = &sound, .startMicros = 500}),
          "slow scheduled sound enters the callback state");
  audio::playback::ActivateScheduledSounds(slowState, 0, 1000, 4, 50);
  require(slowState.playingSoundCount == 1 &&
              slowState.playingSounds[0].outputOffsetFrames == 1,
          "500 chart microseconds take one output frame at 50 percent");

  sound.playing = false;
  AudioCallbackState fastState;
  require(audio::playback::InsertScheduledSound(
              fastState, {.soundData = &sound, .startMicros = 2000}),
          "fast scheduled sound enters the callback state");
  audio::playback::ActivateScheduledSounds(fastState, 0, 1000, 4, 200);
  require(fastState.playingSoundCount == 1 &&
              fastState.playingSounds[0].outputOffsetFrames == 1,
          "two chart milliseconds take one output frame at 200 percent");

  sound.playing = false;
  AudioCallbackState boundedState;
  require(
      audio::playback::InsertScheduledSound(
          boundedState, {.soundData = &sound,
                         .startMicros = std::numeric_limits<long long>::max()}),
      "far-future scheduled sound enters the callback state");
  audio::playback::ActivateScheduledSounds(
      boundedState, std::numeric_limits<long long>::min(), 1000, 4, 100);
  require(boundedState.playingSoundCount == 0 &&
              boundedState.scheduledSoundCount == 1,
          "extreme chart-time deltas remain future events without overflow");
}

void testBusFlowAndMixing() {
  SoundData bgm;
  bgm.channels = 1;
  bgm.outputData = {16384, 16384};
  bgm.outputFrameCount = 2;
  SoundData keysound;
  keysound.channels = 1;
  keysound.outputData = {16384, 16384};
  keysound.outputFrameCount = 2;

  AudioCallbackState state;
  require(
      audio::playback::EnqueueCommand(state, {.type = AudioCommandType::PlayNow,
                                              .soundData = &bgm,
                                              .bus = audio::Bus::Bgm}),
      "BGM play command enters the callback queue");
  require(audio::playback::EnqueueCommand(state,
                                          {.type = AudioCommandType::Schedule,
                                           .soundData = &keysound,
                                           .bus = audio::Bus::Keysound,
                                           .startMicros = 0,
                                           .sequence = 1}),
          "keysound schedule command enters the callback queue");

  audio::playback::DrainCommands(state);
  require(state.playingSoundCount == 1 &&
              state.playingSounds[0].bus == audio::Bus::Bgm,
          "play commands preserve the BGM bus in active state");
  require(state.scheduledSoundCount == 1 &&
              state.scheduledSounds[0].bus == audio::Bus::Keysound,
          "schedule commands preserve the keysound bus in scheduled state");

  audio::playback::ActivateScheduledSounds(state, 0, 48000, 1, 100);
  require(state.playingSoundCount == 2 &&
              state.playingSounds[1].bus == audio::Bus::Keysound,
          "scheduled activation preserves the keysound bus in active state");

  const player_settings::AudioSettings settings{
      .masterVolume = 0.5f, .bgmVolume = 0.5f, .keysoundVolume = 1.0f};
  const auto mappedVolumes = audio::VolumesFromSettings(settings);
  const float bgmGain = audio::EffectiveGain(audio::Bus::Bgm, mappedVolumes);
  const float keysoundGain =
      audio::EffectiveGain(audio::Bus::Keysound, mappedVolumes);
  requireNear(bgmGain, 0.25f, "settings map master and BGM volumes correctly");
  requireNear(keysoundGain, 0.5f,
              "settings map master and keysound volumes correctly");

  std::vector<float> mixBuffer(2, 0.0f);
  audio::playback::MixActiveSounds(state, mixBuffer, 1, 2, bgmGain,
                                   keysoundGain, 100);
  requireNear(mixBuffer[0], 0.3375f,
              "the left channel mixes each voice with its own bus gain");
  requireNear(mixBuffer[1], 0.3375f,
              "the right channel mixes each voice with its own bus gain");
}

void testJukeboxSourceClassificationAndSeekOverlap() {
  const auto chartNote =
      makeScheduledAudioEvent(1000, 10, JukeboxAudioSource::ChartNote);
  const auto background =
      makeScheduledAudioEvent(1000, 11, JukeboxAudioSource::BackgroundNote);
  const auto metronome =
      makeScheduledAudioEvent(1000, 12, JukeboxAudioSource::PrepMetronome);
  const auto clubBeat =
      makeScheduledAudioEvent(1000, 13, JukeboxAudioSource::ClubBeat);
  require(chartNote.bus == audio::Bus::Keysound,
          "chart notes classify as keysounds");
  require(background.bus == audio::Bus::Bgm,
          "background notes classify as BGM");
  require(metronome.bus == audio::Bus::Keysound,
          "preparation metronome clicks classify as keysounds");
  require(clubBeat.bus == audio::Bus::Bgm,
          "Club kick and clap classify as BGM");
  require(audioBusForJukeboxSource(JukeboxAudioSource::DirectKeysound) ==
                  audio::Bus::Keysound &&
              audioBusForJukeboxSource(JukeboxAudioSource::ReplayKeysound) ==
                  audio::Bus::Keysound &&
              audioBusForJukeboxSource(JukeboxAudioSource::SettingsTestTone) ==
                  audio::Bus::Keysound,
          "direct, replay, and settings test sounds classify as keysounds");

  const auto overlapping = makeOverlappingAudioRequest(background, 1250, 1000);
  require(overlapping.has_value() && overlapping->wav == background.wav &&
              overlapping->offsetMicros == 250 &&
              overlapping->bus == audio::Bus::Bgm,
          "seek overlap preserves the scheduled event bus and elapsed time");
  require(!makeOverlappingAudioRequest(background, 2000, 1000).has_value(),
          "seek overlap excludes audio at the end of its duration");
}

void testSchedulerWaitConvertsChartDeltaToWallTime() {
  constexpr long long maxSleepMicros = 250'000;
  require(audio::playback::SchedulerWaitMicrosForChartDelta(
              100'000, {.percent = 50}, maxSleepMicros) == 200'000,
          "50 percent scheduler wait doubles chart delta in wall time");
  require(audio::playback::SchedulerWaitMicrosForChartDelta(
              100'000, {.percent = 100}, maxSleepMicros) == 100'000,
          "100 percent scheduler wait preserves chart delta");
  require(audio::playback::SchedulerWaitMicrosForChartDelta(
              100'000, {.percent = 200}, maxSleepMicros) == 50'000,
          "200 percent scheduler wait halves chart delta in wall time");
  require(audio::playback::SchedulerWaitMicrosForChartDelta(
              1'000'000, {.percent = 50}, maxSleepMicros) == maxSleepMicros,
          "scheduler wall waits retain the maximum idle clamp");
  require(audio::playback::SchedulerWaitMicrosForChartDelta(
              -1, {.percent = 200}, maxSleepMicros) == 0,
          "past scheduler targets remain immediately eligible");
}

} // namespace

int main() {
  try {
    const audio::Volumes volumes{
        .master = 0.5f,
        .bgm = 0.25f,
        .keysound = 0.75f,
    };
    require(audio::EffectiveGain(audio::Bus::Bgm, volumes) == 0.125f,
            "BGM gain multiplies master and BGM volumes");
    require(audio::EffectiveGain(audio::Bus::Keysound, volumes) == 0.375f,
            "keysound gain multiplies master and keysound volumes");

    require(audio::EffectiveGain(audio::Bus::Bgm,
                                 {.master = 2.0f, .bgm = 2.0f}) == 1.0f,
            "gain inputs clamp above one");
    require(audio::EffectiveGain(audio::Bus::Keysound,
                                 {.master = 1.0f, .keysound = -1.0f}) == 0.0f,
            "gain inputs clamp below zero");
    require(audio::EffectiveGain(audio::Bus::Bgm,
                                 {.master = 0.0f, .bgm = 1.0f}) == 0.0f,
            "zero master silences a bus");
    require(audio::EffectiveGain(audio::Bus::Bgm,
                                 {.master = 1.0f, .bgm = 0.0f}) == 0.0f,
            "zero bus volume silences its bus");
    require(
        audio::EffectiveGain(audio::Bus::Keysound,
                             {.master = std::numeric_limits<float>::quiet_NaN(),
                              .keysound = 1.0f}) == 1.0f,
        "non-finite volume falls back to full volume");

    const auto source48 = stereoRamp(480);
    const auto same = audio::ResamplePcm(source48, 2, 48000, 48000);
    require(same == source48, "same-rate resampling returns an exact copy");

    const auto source441 = stereoRamp(441);
    const auto upsampled = audio::ResamplePcm(source441, 2, 44100, 48000);
    require(upsampled.size() == 480 * 2,
            "44.1 to 48 kHz produces the expected stereo frame count");
    require(upsampled.front() == source441.front(),
            "upsampling preserves the first frame");

    const auto downsampled = audio::ResamplePcm(source48, 2, 48000, 44100);
    require(downsampled.size() == 441 * 2,
            "48 to 44.1 kHz produces the expected stereo frame count");
    require(downsampled.front() == source48.front(),
            "downsampling preserves the first frame");

    const std::vector<short> oneStereoFrame{1234, -2345};
    const auto oneFrameDownsampled =
        audio::ResamplePcm(oneStereoFrame, 2, 48000, 44100);
    require(oneFrameDownsampled == oneStereoFrame,
            "a valid one-frame buffer survives downsampling");

    const std::vector<short> fractionalMonoFrames{1000, 2000};
    const auto fractionalUpsampled =
        audio::ResamplePcm(fractionalMonoFrames, 1, 44100, 48000);
    require(fractionalUpsampled.size() == 3,
            "fractional output duration retains its final frame interval");

    require(audio::ResamplePcm({}, 2, 44100, 48000).empty(),
            "empty PCM remains empty");
    require(audio::ResamplePcm(source48, 0, 44100, 48000).empty(),
            "invalid channel count is rejected");
    require(audio::ResamplePcm(source48, 2, 0, 48000).empty(),
            "invalid source rate is rejected");
    require(audio::ResamplePcm(source48, 2, 48000, 0).empty(),
            "invalid target rate is rejected");
    const std::vector<short> partialStereoFrame{1, 2, 3};
    require(audio::ResamplePcm(partialStereoFrame, 2, 48000, 48000).empty(),
            "same-rate conversion rejects partial interleaved PCM");
    require(audio::ResamplePcm(partialStereoFrame, 2, 44100, 48000).empty(),
            "rate conversion rejects partial interleaved PCM");

    const audio::PlaybackRate doubleSpeed{.percent = 200};
    require(chart_audio::outputTimeMicros(2'000'000, doubleSpeed) ==
                1'000'000,
            "offline chart audio compresses event time at double speed");
    requireNear(static_cast<float>(chart_audio::sourceFramesPerOutputFrame(
                    44100, doubleSpeed)),
                2.0f,
                "offline chart audio advances two source frames at double "
                "speed");

    const ScheduledAudioEvent defaultEvent;
    require(defaultEvent.wav == bms_parser::Parser::NoWav &&
                defaultEvent.bus == audio::Bus::Bgm,
            "scheduled audio defaults to no WAV on the BGM bus");
    const ScheduledAudioEvent keysoundEvent{
        .timeMicros = 123,
        .wav = 42,
        .bus = audio::Bus::Keysound,
    };
    require(keysoundEvent.timeMicros == 123 && keysoundEvent.wav == 42 &&
                keysoundEvent.bus == audio::Bus::Keysound,
            "scheduled audio retains explicit keysound classification");

    testRateTransitionRegeneratesAndRemapsEveryFrameDomain();
    testStoppedQueryInterpretationPreservesErrors();
    testUnknownBackendStateCannotPublishRateTransition();
    testStopDrainFailureCannotPublishRateTransition();
    testPostStopStateErrorCannotPublishRateTransition();
    testConfirmedDrainPublishesThenRestartsAtNewRate();
    testAlreadyRunningAtTargetRateDoesNotRestart();
    testStartFailureCannotPublishRunningState();
    testPostStartQueryFailureCannotPublishRunningState();
    testStopFailureCannotClearCallbackState();
    testConfirmedStopClearsCallbackStateAfterDrain();
    testPitchShiftMixerUsesQ32Interpolation();
    testQ32ActiveCursorRejectsUnrepresentableStartFrame();
    testScheduledOffsetsUseInversePlaybackRate();
    testBusFlowAndMixing();
    testJukeboxSourceClassificationAndSeekOverlap();
    testSchedulerWaitConvertsChartDeltaToWallTime();

    return 0;
  } catch (const std::exception &error) {
    std::cerr << "audio_mix_tests: " << error.what() << '\n';
    return 1;
  }
}
