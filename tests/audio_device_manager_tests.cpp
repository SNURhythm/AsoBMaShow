#include "audio/AudioDeviceManager.h"

#include <cmath>
#include <deque>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

audio::Capabilities selectableCapabilities() {
  return {
      .canSelectOutputDevice = true,
      .canSelectSampleRate = true,
      .canSelectBufferFrames = true,
      .outputDevices = {{.id = "default",
                         .name = "Default Output",
                         .isDefault = true,
                         .sampleRates = {44100, 48000},
                         .bufferFrames = {0, 128, 256}},
                        {.id = "usb",
                         .name = "USB DAC",
                         .sampleRates = {48000, 96000},
                         .bufferFrames = {0, 64, 128}}},
  };
}

player_settings::AudioSettings baseSettings() {
  return {.outputDeviceId = "default",
          .requestedSampleRate = 44100,
          .requestedBufferFrames = 128,
          .masterVolume = 1.0F,
          .bgmVolume = 1.0F,
          .keysoundVolume = 1.0F};
}

class FakeRuntime final : public audio::IAudioRuntime {
public:
  audio::Capabilities capabilitiesValue = selectableCapabilities();
  audio::RuntimeState state{
      .request = {.deviceId = "default",
                  .sampleRate = 44100,
                  .bufferFrames = 128},
      .effectiveSampleRate = 44100,
      .effectiveBufferFrames = 128,
      .effectiveLatencyMs = 128000.0 / 44100.0,
  };
  std::deque<bool> restartResults{true};
  std::deque<bool> restoreResults{true};
  std::vector<std::string> *events = nullptr;
  audio::Volumes volumes;

  audio::Capabilities capabilities() const override {
    return capabilitiesValue;
  }
  audio::RuntimeState runtimeState() const override { return state; }

  bool restart(const audio::StreamRequest &request,
               std::string &errorMessage) override {
    record("restart");
    const bool succeeds = pop(restartResults, true);
    if (!succeeds) {
      errorMessage = "candidate open failed";
      return false;
    }
    state.request = request;
    state.effectiveSampleRate =
        request.sampleRate == 0 ? 48000 : request.sampleRate;
    state.effectiveBufferFrames =
        request.bufferFrames == 0 ? 256 : request.bufferFrames;
    state.effectiveLatencyMs =
        1000.0 * state.effectiveBufferFrames / state.effectiveSampleRate;
    return true;
  }

  bool restore(const audio::RuntimeState &previous,
               std::string &errorMessage) override {
    record("restore-runtime");
    const bool succeeds = pop(restoreResults, true);
    if (!succeeds) {
      errorMessage = "previous stream could not reopen";
      return false;
    }
    state = previous;
    return true;
  }

  void setVolumes(const audio::Volumes &newVolumes) override {
    record("set-volumes");
    volumes = newVolumes;
  }

private:
  static bool pop(std::deque<bool> &values, bool fallback) {
    if (values.empty()) {
      return fallback;
    }
    const bool value = values.front();
    values.pop_front();
    return value;
  }

  void record(std::string event) {
    if (events != nullptr) {
      events->push_back(std::move(event));
    }
  }
};

class FakePlayback final : public audio::IPlaybackSession {
public:
  audio::PlaybackSnapshot snapshot{
      .active = true, .paused = false, .positionMicros = 1'234'567};
  std::deque<bool> restoreResults{true};
  std::vector<std::string> *events = nullptr;
  int suspendCount = 0;
  int stoppedCount = 0;

  audio::PlaybackSnapshot suspendAndDrain() override {
    record("suspend");
    ++suspendCount;
    return snapshot;
  }

  bool restorePlayback(const audio::PlaybackSnapshot &candidate,
                       std::string &errorMessage) override {
    record("restore-playback");
    require(candidate.positionMicros == snapshot.positionMicros,
            "playback restore keeps the exact position");
    const bool succeeds = pop(restoreResults, true);
    if (!succeeds) {
      errorMessage = "playback resume failed";
    }
    return succeeds;
  }

  void leavePlaybackStopped() override {
    record("leave-stopped");
    ++stoppedCount;
  }

private:
  static bool pop(std::deque<bool> &values, bool fallback) {
    if (values.empty()) {
      return fallback;
    }
    const bool value = values.front();
    values.pop_front();
    return value;
  }

  void record(std::string event) {
    if (events != nullptr) {
      events->push_back(std::move(event));
    }
  }
};

void testRejectsUnsupportedRequestBeforeSuspension() {
  FakeRuntime runtime;
  FakePlayback playback;
  audio::AudioDeviceManager manager(runtime, playback, baseSettings());

  auto candidate = baseSettings();
  candidate.outputDeviceId = "missing:device";
  candidate.masterVolume = 0.25F;
  const auto result = manager.apply(candidate);

  require(result.status == audio::ApplyStatus::Unsupported,
          "unknown device is rejected as unsupported");
  require(playback.suspendCount == 0,
          "unsupported request is rejected before playback suspension");
  require(manager.lastWorkingSettings() == baseSettings(),
          "unsupported imported intent does not replace working settings");
  require(result.effective == runtime.runtimeState(),
          "unsupported result still reports effective runtime state");
}

void testValidatesDeviceSpecificRateAndBuffer() {
  FakeRuntime runtime;
  FakePlayback playback;
  audio::AudioDeviceManager manager(runtime, playback, baseSettings());

  auto candidate = baseSettings();
  candidate.outputDeviceId = "usb";
  candidate.requestedSampleRate = 44100;
  require(manager.apply(candidate).status == audio::ApplyStatus::Unsupported,
          "rate must be supported by the selected device");
  candidate.requestedSampleRate = 48000;
  candidate.requestedBufferFrames = 256;
  require(manager.apply(candidate).status == audio::ApplyStatus::Unsupported,
          "buffer must be supported by the selected device");
  require(playback.suspendCount == 0,
          "invalid device-specific choices never suspend playback");
}

void testRejectsEmptySelectableCapabilitySetBeforeSuspension() {
  FakeRuntime runtime;
  FakePlayback playback;
  runtime.capabilitiesValue.outputDevices.clear();
  audio::AudioDeviceManager manager(runtime, playback, baseSettings());

  const auto result = manager.apply(baseSettings());
  require(result.status == audio::ApplyStatus::Unsupported,
          "selectable runtime with no outputs rejects the request");
  require(playback.suspendCount == 0,
          "missing output device is detected before suspension");
}

void testVolumeOnlyApplyDoesNotRestart() {
  FakeRuntime runtime;
  FakePlayback playback;
  std::vector<std::string> events;
  runtime.events = &events;
  playback.events = &events;
  audio::AudioDeviceManager manager(runtime, playback, baseSettings());

  auto candidate = baseSettings();
  candidate.masterVolume = 0.5F;
  candidate.bgmVolume = 0.25F;
  candidate.keysoundVolume = 0.75F;
  const auto result = manager.apply(candidate);

  require(result.status == audio::ApplyStatus::Applied,
          "volume-only request applies");
  require(events == std::vector<std::string>{"set-volumes"},
          "volume-only request neither suspends nor restarts");
  require(std::fabs(runtime.volumes.master - 0.5F) < 0.00001F &&
              std::fabs(runtime.volumes.bgm - 0.25F) < 0.00001F &&
              std::fabs(runtime.volumes.keysound - 0.75F) < 0.00001F,
          "volume-only request updates every audio bus");
  require(manager.lastWorkingSettings() == candidate,
          "successful volume apply becomes last working settings");
}

void testSuccessfulRestartRestoresPlaybackInOrder() {
  FakeRuntime runtime;
  FakePlayback playback;
  std::vector<std::string> events;
  runtime.events = &events;
  playback.events = &events;
  audio::AudioDeviceManager manager(runtime, playback, baseSettings());

  auto candidate = baseSettings();
  candidate.outputDeviceId = "usb";
  candidate.requestedSampleRate = 48000;
  candidate.requestedBufferFrames = 64;
  const auto result = manager.apply(candidate);

  require(result.status == audio::ApplyStatus::Applied,
          "valid stream request applies");
  require(result.playbackResumed, "active playback is reported resumed");
  require(events == std::vector<std::string>{"suspend", "restart",
                                             "restore-playback", "set-volumes"},
          "successful transaction suspends, restarts, restores, then commits "
          "volumes");
  require(manager.lastWorkingSettings() == candidate,
          "successful stream request replaces last working settings");
  require(result.effective.request.deviceId == "usb" &&
              result.effective.effectiveSampleRate == 48000 &&
              result.effective.effectiveBufferFrames == 64,
          "successful result exposes the effective stream");
}

void testOpenFailureRollsBackAndResumesPreviousPlayback() {
  FakeRuntime runtime;
  FakePlayback playback;
  std::vector<std::string> events;
  runtime.events = &events;
  playback.events = &events;
  runtime.restartResults = {false};
  audio::AudioDeviceManager manager(runtime, playback, baseSettings());

  auto candidate = baseSettings();
  candidate.requestedSampleRate = 48000;
  const auto result = manager.apply(candidate);

  require(result.status == audio::ApplyStatus::FailedRolledBack,
          "failed open reports successful rollback");
  require(events == std::vector<std::string>{"suspend", "restart",
                                             "restore-runtime",
                                             "restore-playback"},
          "open failure restores the previous stream before playback");
  require(result.playbackResumed,
          "rollback reports restoration of active playback");
  require(manager.lastWorkingSettings() == baseSettings(),
          "failed candidate never replaces working settings");
}

void testPlaybackResumeFailureRollsBackNewStream() {
  FakeRuntime runtime;
  FakePlayback playback;
  std::vector<std::string> events;
  runtime.events = &events;
  playback.events = &events;
  playback.restoreResults = {false, true};
  audio::AudioDeviceManager manager(runtime, playback, baseSettings());

  auto candidate = baseSettings();
  candidate.requestedSampleRate = 48000;
  const auto result = manager.apply(candidate);

  require(result.status == audio::ApplyStatus::FailedRolledBack,
          "resume failure reports rollback when old playback recovers");
  require(
      events == std::vector<std::string>{"suspend", "restart",
                                         "restore-playback", "restore-runtime",
                                         "restore-playback"},
      "resume failure returns to the old stream and retries exact snapshot");
  require(result.playbackResumed,
          "successful rollback retry reports playback resumed");
  require(result.effective.request.sampleRate == 44100,
          "effective state returns to the previous stream");
}

void testDoubleFailureLeavesPlaybackStopped() {
  FakeRuntime runtime;
  FakePlayback playback;
  std::vector<std::string> events;
  runtime.events = &events;
  playback.events = &events;
  playback.restoreResults = {false};
  runtime.restoreResults = {false};
  audio::AudioDeviceManager manager(runtime, playback, baseSettings());

  auto candidate = baseSettings();
  candidate.requestedSampleRate = 48000;
  const auto result = manager.apply(candidate);

  require(result.status == audio::ApplyStatus::FailedStopped,
          "double failure reports stopped playback");
  require(!result.playbackResumed && playback.stoppedCount == 1,
          "unrecoverable transaction explicitly leaves playback stopped");
  require(events ==
              std::vector<std::string>{"suspend", "restart", "restore-playback",
                                       "restore-runtime", "leave-stopped"},
          "double failure has deterministic stop ordering");
  require(manager.lastWorkingSettings() == baseSettings(),
          "unrecoverable candidate still does not become last working");
}

void testFailedDrainNeverTouchesTheRuntime() {
  FakeRuntime runtime;
  FakePlayback playback;
  std::vector<std::string> events;
  runtime.events = &events;
  playback.events = &events;
  playback.snapshot.valid = false;
  audio::AudioDeviceManager manager(runtime, playback, baseSettings());

  auto candidate = baseSettings();
  candidate.requestedSampleRate = 48000;
  const auto result = manager.apply(candidate);

  require(result.status == audio::ApplyStatus::FailedStopped,
          "failed drain is unrecoverable without touching the stream");
  require(events == std::vector<std::string>{"suspend", "leave-stopped"},
          "invalid snapshot never opens a candidate stream");
}

void testFixedRuntimeAcceptsOnlyDefaultStreamIntent() {
  FakeRuntime runtime;
  FakePlayback playback;
  runtime.capabilitiesValue = {};
  runtime.state.request = {};
  audio::AudioDeviceManager manager(runtime, playback, {});

  player_settings::AudioSettings defaults;
  defaults.masterVolume = 0.4F;
  require(manager.apply(defaults).status == audio::ApplyStatus::Applied,
          "fixed mobile runtime still accepts volume changes");

  auto unsupported = defaults;
  unsupported.requestedBufferFrames = 128;
  require(manager.apply(unsupported).status == audio::ApplyStatus::Unsupported,
          "fixed runtime rejects explicit unsupported stream selection");
  require(playback.suspendCount == 0,
          "fixed-runtime rejection happens before suspension");
}

} // namespace

int main() {
  testRejectsUnsupportedRequestBeforeSuspension();
  testValidatesDeviceSpecificRateAndBuffer();
  testRejectsEmptySelectableCapabilitySetBeforeSuspension();
  testVolumeOnlyApplyDoesNotRestart();
  testSuccessfulRestartRestoresPlaybackInOrder();
  testOpenFailureRollsBackAndResumesPreviousPlayback();
  testPlaybackResumeFailureRollsBackNewStream();
  testDoubleFailureLeavesPlaybackStopped();
  testFailedDrainNeverTouchesTheRuntime();
  testFixedRuntimeAcceptsOnlyDefaultStreamIntent();
  return 0;
}
