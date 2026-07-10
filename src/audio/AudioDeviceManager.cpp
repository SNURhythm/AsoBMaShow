#include "AudioDeviceManager.h"

#include <algorithm>
#include <string_view>

namespace audio {
namespace {

StreamRequest requestFrom(const player_settings::AudioSettings &settings) {
  return {.deviceId = settings.outputDeviceId,
          .sampleRate = settings.requestedSampleRate,
          .bufferFrames = settings.requestedBufferFrames};
}

Volumes volumesFrom(const player_settings::AudioSettings &settings) {
  return {.master = settings.masterVolume,
          .bgm = settings.bgmVolume,
          .keysound = settings.keysoundVolume};
}

template <typename Value>
bool contains(const std::vector<Value> &values, const Value &target) {
  return std::find(values.begin(), values.end(), target) != values.end();
}

void appendMessage(std::string &message, std::string_view addition) {
  if (addition.empty()) {
    return;
  }
  if (!message.empty()) {
    message += "; ";
  }
  message += addition;
}

bool sameVolumes(const Volumes &left, const Volumes &right) {
  return left.master == right.master && left.bgm == right.bgm &&
         left.keysound == right.keysound;
}

void updateVolumeFields(player_settings::AudioSettings &settings,
                        const Volumes &volumes) {
  settings.masterVolume = volumes.master;
  settings.bgmVolume = volumes.bgm;
  settings.keysoundVolume = volumes.keysound;
}

} // namespace

AudioDeviceManager::AudioDeviceManager(
    IAudioRuntime &runtime, IPlaybackSession &playback,
    player_settings::AudioSettings lastWorkingSettings)
    : runtime_(runtime), playback_(playback) {
  (void)lastWorkingSettings;
  const StreamRequest effectiveRequest = runtime_.runtimeState().request;
  lastWorkingSettings_.outputDeviceId = effectiveRequest.deviceId;
  lastWorkingSettings_.requestedSampleRate = effectiveRequest.sampleRate;
  lastWorkingSettings_.requestedBufferFrames = effectiveRequest.bufferFrames;
  updateVolumeFields(lastWorkingSettings_, appliedVolumes_);
}

Capabilities AudioDeviceManager::capabilities() const {
  return runtime_.capabilities();
}

const player_settings::AudioSettings &
AudioDeviceManager::lastWorkingSettings() const {
  return lastWorkingSettings_;
}

const ApplyResult &AudioDeviceManager::lastApplyResult() const {
  return lastApplyResult_;
}

ApplyResult AudioDeviceManager::remember(ApplyResult result) {
  lastApplyResult_ = result;
  return result;
}

ApplyResult
AudioDeviceManager::apply(const player_settings::AudioSettings &candidate) {
  const RuntimeState previousRuntime = runtime_.runtimeState();
  const StreamRequest request = requestFrom(candidate);
  const Volumes candidateVolumes = volumesFrom(candidate);
  if (!sameVolumes(candidateVolumes, appliedVolumes_)) {
    runtime_.setVolumes(candidateVolumes);
    appliedVolumes_ = candidateVolumes;
  }
  updateVolumeFields(lastWorkingSettings_, candidateVolumes);
  std::string validationMessage;
  if (!validateRequest(request, runtime_.capabilities(), validationMessage)) {
    return remember({.status = ApplyStatus::Unsupported,
                     .effective = previousRuntime,
                     .message = std::move(validationMessage)});
  }

  if (request == previousRuntime.request) {
    lastWorkingSettings_ = candidate;
    return remember(
        {.status = ApplyStatus::Applied, .effective = runtime_.runtimeState()});
  }

  const PlaybackSnapshot snapshot = playback_.suspendAndDrain();
  if (!snapshot.valid) {
    playback_.leavePlaybackStopped();
    return remember({
        .status = ApplyStatus::FailedStopped,
        .effective = runtime_.runtimeState(),
        .message = "Playback could not be suspended and drained",
    });
  }
  std::string restartError;
  if (!runtime_.restart(request, restartError)) {
    if (restartError.empty()) {
      restartError = "Audio stream restart failed";
    }
    return remember(
        rollback(previousRuntime, snapshot, std::move(restartError)));
  }

  std::string playbackError;
  if (!playback_.restorePlayback(snapshot, playbackError)) {
    if (playbackError.empty()) {
      playbackError = "Playback could not resume on the candidate stream";
    }
    return remember(
        rollback(previousRuntime, snapshot, std::move(playbackError)));
  }

  lastWorkingSettings_ = candidate;
  return remember({.status = ApplyStatus::Applied,
                   .effective = runtime_.runtimeState(),
                   .playbackResumed = snapshot.active});
}

bool AudioDeviceManager::validateRequest(const StreamRequest &request,
                                         const Capabilities &capabilities,
                                         std::string &message) const {
  const DeviceInfo *selectedDevice = nullptr;
  if (!request.deviceId.empty()) {
    if (!capabilities.canSelectOutputDevice) {
      message = "Output-device selection is unsupported";
      return false;
    }
    const auto selected = std::find_if(capabilities.outputDevices.begin(),
                                       capabilities.outputDevices.end(),
                                       [&](const DeviceInfo &device) {
                                         return device.id == request.deviceId;
                                       });
    if (selected == capabilities.outputDevices.end()) {
      message = "Requested output device is unavailable";
      return false;
    }
    selectedDevice = &*selected;
  } else if (!capabilities.outputDevices.empty()) {
    const auto defaultDevice = std::find_if(
        capabilities.outputDevices.begin(), capabilities.outputDevices.end(),
        [](const DeviceInfo &device) { return device.isDefault; });
    selectedDevice = defaultDevice != capabilities.outputDevices.end()
                         ? &*defaultDevice
                         : &capabilities.outputDevices.front();
  }
  if (capabilities.canSelectOutputDevice && selectedDevice == nullptr) {
    message = "No output device is available";
    return false;
  }

  if (request.sampleRate != 0) {
    if (!capabilities.canSelectSampleRate) {
      message = "Sample-rate selection is unsupported";
      return false;
    }
    if (selectedDevice != nullptr &&
        !contains(selectedDevice->sampleRates, request.sampleRate)) {
      message = "Requested sample rate is unavailable for the output device";
      return false;
    }
  }

  if (request.bufferFrames != 0) {
    if (!capabilities.canSelectBufferFrames) {
      message = "Buffer-size selection is unsupported";
      return false;
    }
    if (selectedDevice != nullptr &&
        !contains(selectedDevice->bufferFrames, request.bufferFrames)) {
      message = "Requested buffer size is unavailable for the output device";
      return false;
    }
  }
  return true;
}

ApplyResult AudioDeviceManager::rollback(const RuntimeState &previousRuntime,
                                         const PlaybackSnapshot &snapshot,
                                         std::string failureMessage) {
  std::string restoreRuntimeError;
  if (!runtime_.restore(previousRuntime, restoreRuntimeError)) {
    appendMessage(failureMessage,
                  restoreRuntimeError.empty()
                      ? "Previous audio stream could not be restored"
                      : restoreRuntimeError);
    playback_.leavePlaybackStopped();
    return {.status = ApplyStatus::FailedStopped,
            .effective = runtime_.runtimeState(),
            .message = std::move(failureMessage)};
  }

  std::string restorePlaybackError;
  if (!playback_.restorePlayback(snapshot, restorePlaybackError)) {
    appendMessage(failureMessage,
                  restorePlaybackError.empty()
                      ? "Playback could not resume after rollback"
                      : restorePlaybackError);
    playback_.leavePlaybackStopped();
    return {.status = ApplyStatus::FailedStopped,
            .effective = runtime_.runtimeState(),
            .message = std::move(failureMessage)};
  }

  return {.status = ApplyStatus::FailedRolledBack,
          .effective = runtime_.runtimeState(),
          .playbackResumed = snapshot.active,
          .message = std::move(failureMessage)};
}

} // namespace audio
