#pragma once

#include "AudioBackend.h"
#include "PlaybackRate.h"
#include "../settings/AudioVideoSettings.h"

#include <string>

namespace audio {

struct PlaybackSnapshot {
  bool valid = true;
  bool active = false;
  bool paused = false;
  long long positionMicros = 0;
  PlaybackRate rate;
  bool operator==(const PlaybackSnapshot &) const = default;
};

class IAudioRuntime {
public:
  virtual ~IAudioRuntime() = default;
  [[nodiscard]] virtual Capabilities capabilities() const = 0;
  [[nodiscard]] virtual RuntimeState runtimeState() const = 0;
  virtual bool restart(const StreamRequest &request,
                       std::string &errorMessage) = 0;
  virtual bool restore(const RuntimeState &previous,
                       std::string &errorMessage) = 0;
  virtual void setVolumes(const Volumes &volumes) = 0;
};

class IPlaybackSession {
public:
  virtual ~IPlaybackSession() = default;
  virtual PlaybackSnapshot suspendAndDrain() = 0;
  virtual bool restorePlayback(const PlaybackSnapshot &snapshot,
                               std::string &errorMessage) = 0;
  virtual void leavePlaybackStopped() = 0;
};

enum class ApplyStatus {
  Applied,
  Unsupported,
  FailedRolledBack,
  FailedStopped,
};

struct ApplyResult {
  ApplyStatus status = ApplyStatus::Unsupported;
  RuntimeState effective;
  bool playbackResumed = false;
  std::string message;
  bool operator==(const ApplyResult &) const = default;
};

class AudioDeviceManager {
public:
  AudioDeviceManager(IAudioRuntime &runtime, IPlaybackSession &playback,
                     player_settings::AudioSettings lastWorkingSettings);

  [[nodiscard]] Capabilities capabilities() const;
  [[nodiscard]] const player_settings::AudioSettings &
  lastWorkingSettings() const;
  [[nodiscard]] const ApplyResult &lastApplyResult() const;
  ApplyResult apply(const player_settings::AudioSettings &candidate);

private:
  [[nodiscard]] bool validateRequest(const StreamRequest &request,
                                     const Capabilities &capabilities,
                                     std::string &message) const;
  ApplyResult rollback(const RuntimeState &previousRuntime,
                       const PlaybackSnapshot &snapshot,
                       std::string failureMessage);
  ApplyResult remember(ApplyResult result);

  IAudioRuntime &runtime_;
  IPlaybackSession &playback_;
  player_settings::AudioSettings lastWorkingSettings_;
  Volumes appliedVolumes_;
  ApplyResult lastApplyResult_;
};

} // namespace audio
