#pragma once

#include <vector>

namespace prep_metronome_audio {

std::vector<short> makeClick(bool accent, int sampleRate, int channels);

} // namespace prep_metronome_audio
