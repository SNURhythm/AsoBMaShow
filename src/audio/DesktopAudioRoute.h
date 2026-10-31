#pragma once
#include <optional>

namespace audio {
// PortAudio's stream is opaque (void). Unknown routes must not reset a profile.
std::optional<bool> desktopOutputIsBluetooth(void *stream, bool refresh = false);
}
