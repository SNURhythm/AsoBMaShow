#include "MidiInputBackendFactory.h"

#include "MidiPlatformBackends.h"

#include <utility>

std::unique_ptr<IInputBackend>
makeMidiInputBackend(input::InputBackendSink sink) {
#if defined(__APPLE__)
  return makeCoreMidiInputBackend(std::move(sink));
#elif defined(_WIN32)
  return makeWinMidiInputBackend(std::move(sink));
#elif defined(__ANDROID__)
  return makeAndroidMidiInputBackend(std::move(sink));
#else
  return makeUnsupportedMidiInputBackend(std::move(sink));
#endif
}
