#pragma once

#include "AudioWrapper.h"
#include "SharedAudioOutput.h"

namespace audio {

// Application lifetime owns the device. Jukebox and scene sound owners borrow
// separate processors: their stop/unload, queues, handles and clocks are local.
class ApplicationAudio final : public IAudioRuntime {
public:
  explicit ApplicationAudio(Stopwatch *gameplayClock);
  ApplicationAudio(Stopwatch *gameplayClock, std::unique_ptr<IBackendFactory> factory);
  AudioWrapper &gameplay() noexcept { return gameplay_; }
  AudioWrapper &systemSounds() noexcept { return system_; }
  Capabilities capabilities() const override;
  RuntimeState runtimeState() const override;
  std::optional<bool> outputIsBluetooth(bool refresh = false) const override;
  bool restart(const StreamRequest &request, std::string &error) override;
  bool restore(const RuntimeState &previous, std::string &error) override;
  void setVolumes(const Volumes &volumes) override;
  void setMonoAudio(bool enabled) override;
  void setApplicationSuspended(bool suspended) noexcept;

private:
  SharedAudioOutput output_;
  Stopwatch systemClock_; // Never follows the chart; System voices run while stopped.
  AudioWrapper gameplay_;
  AudioWrapper system_;
};
}
