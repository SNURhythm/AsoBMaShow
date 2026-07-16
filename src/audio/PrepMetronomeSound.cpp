#include "PrepMetronomeSound.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>

namespace prep_metronome_audio {

std::vector<short> makeClick(bool accent, int sampleRate, int channels) {
  if (sampleRate <= 0 || channels <= 0) {
    return {};
  }

  constexpr double kClickSeconds = 0.045;
  constexpr double kPi = 3.14159265358979323846;
  const double frequency = accent ? 1760.0 : 1100.0;
  const double amplitude = accent ? 0.65 : 0.5;
  const int frames = static_cast<int>(
      std::lround(static_cast<double>(sampleRate) * kClickSeconds));
  std::vector<short> pcm(static_cast<std::size_t>(frames) *
                         static_cast<std::size_t>(channels));
  for (int frame = 0; frame < frames; ++frame) {
    const double time =
        static_cast<double>(frame) / static_cast<double>(sampleRate);
    const double envelope = std::exp(-time * 90.0);
    const double sample =
        std::sin(2.0 * kPi * frequency * time) * envelope * amplitude;
    const auto value = static_cast<short>(std::clamp(sample, -1.0, 1.0) *
                                          static_cast<double>(INT16_MAX));
    for (int channel = 0; channel < channels; ++channel) {
      pcm[static_cast<std::size_t>(frame) * static_cast<std::size_t>(channels) +
          static_cast<std::size_t>(channel)] = value;
    }
  }
  return pcm;
}

} // namespace prep_metronome_audio
