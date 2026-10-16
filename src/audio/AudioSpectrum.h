#pragma once

#include <array>
#include <cstddef>

namespace audio {
inline constexpr std::size_t kSpectrumBandCount = 16;
// Wider logarithmic buckets aggregate neighboring FFT frequencies, low to
// high, normalized to [0, 1].
using Spectrum = std::array<float, kSpectrumBandCount>;
}
