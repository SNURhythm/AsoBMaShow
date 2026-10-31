#pragma once

#include "AudioBackend.h"

#include <cstddef>
#include <memory>

namespace audio {

// One native output with independently drainable software submixes. Factories
// and their streams retain the graph; a stream's stop never stops the device.
class SharedAudioOutput {
public:
  static constexpr std::size_t kSubmixCount = 2;
  explicit SharedAudioOutput(std::unique_ptr<IBackendFactory> factory);
  ~SharedAudioOutput();
  SharedAudioOutput(const SharedAudioOutput &) = delete;
  SharedAudioOutput &operator=(const SharedAudioOutput &) = delete;

  std::unique_ptr<IBackendFactory> createSubmixFactory(std::size_t index);
  [[nodiscard]] Capabilities capabilities() const;
  [[nodiscard]] RuntimeState runtimeState() const;
  [[nodiscard]] std::optional<bool> outputIsBluetooth(bool refresh = false) const;

  // Caller suspends both processors first. Open leaves native output stopped;
  // all processors must commit their new PCM formats before start().
  bool reopen(const StreamRequest &request, std::string &error);
  bool start(std::string &error);

private:
  struct State;
  class Submix;
  class Factory;
  std::shared_ptr<State> state_;
};
}
