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

void appendMessage(i18n::Text &message, i18n::Text addition) {
  if (addition.empty()) {
    return;
  }
  message = message.empty() ? std::move(addition)
                           : i18n::message("settings.audio_video.audio.failure_detail",
                                           {{"first", message}, {"second", addition}});
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
  i18n::Text validationMessage;
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
        .message = i18n::message("settings.audio_video.audio.drain_failed"),
    });
  }
  std::string restartError;
  if (!runtime_.restart(request, restartError)) {
    return remember(rollback(previousRuntime, snapshot,
        restartError.empty() ? i18n::message("settings.audio_video.audio.restart_failed")
                             : i18n::Text(std::move(restartError))));
  }

  std::string playbackError;
  if (!playback_.restorePlayback(snapshot, playbackError)) {
    return remember(rollback(previousRuntime, snapshot,
        playbackError.empty() ? i18n::message("settings.audio_video.audio.candidate_resume_failed")
                              : i18n::Text(std::move(playbackError))));
  }

  lastWorkingSettings_ = candidate;
  return remember({.status = ApplyStatus::Applied,
                   .effective = runtime_.runtimeState(),
                   .playbackResumed = snapshot.active});
}

bool AudioDeviceManager::validateRequest(const StreamRequest &request,
                                         const Capabilities &capabilities,
                                         i18n::Text &message) const {
  const DeviceInfo *selectedDevice = nullptr;
  if (!request.deviceId.empty()) {
    if (!capabilities.canSelectOutputDevice) {
      message = i18n::message("settings.audio_video.audio.device_selection_unsupported");
      return false;
    }
    const auto selected = std::find_if(capabilities.outputDevices.begin(),
                                       capabilities.outputDevices.end(),
                                       [&](const DeviceInfo &device) {
                                         return device.id == request.deviceId;
                                       });
    if (selected == capabilities.outputDevices.end()) {
      message = i18n::message("settings.audio_video.audio.device_unavailable");
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
    message = i18n::message("settings.audio_video.audio.no_device");
    return false;
  }

  if (request.sampleRate != 0) {
    if (!capabilities.canSelectSampleRate) {
      message = i18n::message("settings.audio_video.audio.rate_selection_unsupported");
      return false;
    }
    if (selectedDevice != nullptr &&
        !contains(selectedDevice->sampleRates, request.sampleRate)) {
      message = i18n::message("settings.audio_video.audio.rate_unavailable");
      return false;
    }
  }

  if (request.bufferFrames != 0) {
    if (!capabilities.canSelectBufferFrames) {
      message = i18n::message("settings.audio_video.audio.buffer_selection_unsupported");
      return false;
    }
    if (selectedDevice != nullptr &&
        !contains(selectedDevice->bufferFrames, request.bufferFrames)) {
      message = i18n::message("settings.audio_video.audio.buffer_unavailable");
      return false;
    }
  }
  return true;
}

ApplyResult AudioDeviceManager::rollback(const RuntimeState &previousRuntime,
                                         const PlaybackSnapshot &snapshot,
                                         i18n::Text failureMessage) {
  std::string restoreRuntimeError;
  if (!runtime_.restore(previousRuntime, restoreRuntimeError)) {
    appendMessage(failureMessage,
                  restoreRuntimeError.empty()
                      ? i18n::message("settings.audio_video.audio.stream_restore_failed")
                      : i18n::Text(restoreRuntimeError));
    playback_.leavePlaybackStopped();
    return {.status = ApplyStatus::FailedStopped,
            .effective = runtime_.runtimeState(),
            .message = std::move(failureMessage)};
  }

  std::string restorePlaybackError;
  if (!playback_.restorePlayback(snapshot, restorePlaybackError)) {
    appendMessage(failureMessage,
                  restorePlaybackError.empty()
                      ? i18n::message("settings.audio_video.audio.rollback_resume_failed")
                      : i18n::Text(restorePlaybackError));
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
