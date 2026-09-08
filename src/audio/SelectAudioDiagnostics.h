#pragma once

#include <string>
#include <string_view>

// Diagnostics for the music-select audio path. Writes timestamped lines to
// `<Documents>/select-audio.log` (app Documents: visible in the Files app on
// iOS/Android, `~/AsoBMaShow` on desktop) so the select SE / BGM / preview
// audio flow can be diagnosed on-device without console access. All writes are
// best-effort and never fail the caller.
namespace audio::diag {

// Appends one line, best-effort.
void SelectAudioLog(std::string_view message);

} // namespace audio::diag