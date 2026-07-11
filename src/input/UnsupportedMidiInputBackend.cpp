#include "MidiPlatformBackends.h"

#if !defined(__APPLE__) && !defined(_WIN32) && !defined(__ANDROID__)

#include "QueuedMidiInputBackend.h"

#include <utility>

namespace {

class UnsupportedMidiInputBackend final : public QueuedMidiInputBackend {
public:
  explicit UnsupportedMidiInputBackend(input::InputBackendSink sink)
      : QueuedMidiInputBackend(std::move(sink)) {}

  bool start(std::string &errorMessage) override {
    errorMessage = "Native MIDI input is unavailable on this platform.";
    closeQueue();
    return false;
  }

  void stop() override { closeQueue(); }
};

} // namespace

std::unique_ptr<IInputBackend>
makeUnsupportedMidiInputBackend(input::InputBackendSink sink) {
  return std::make_unique<UnsupportedMidiInputBackend>(std::move(sink));
}

#endif
