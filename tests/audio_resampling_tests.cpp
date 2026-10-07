#include "audio/AudioMix.h"

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
void checkSamples() {
  // At frame 1 the source position is 147/160: -73 + 80*147/160 = +0.5.
  // Opposite stereo channels must round both exact ties away from zero.
  const std::vector<short> ties{-73, 73, 7, -7};
  require(audio::ResamplePcm(ties, 2, 44100, 48000) ==
              std::vector<short>{-73, 73, 1, -1, 7, -7},
          "fractional-rate half samples must round away from zero");
  require(audio::ResamplePcm(std::vector<short>{-32768, 32767}, 1, 1, 2) ==
              std::vector<short>{-32768, -1, 32767, 32767},
          "interpolation must preserve signed endpoints and clamp the final frame");
  require(audio::ResamplePcm(std::vector<short>{0, 100, 200, 300, 400}, 1, 3, 2) ==
              std::vector<short>{0, 150, 300, 400},
          "downsampling must advance whole frames and retain the final interval");
  // Independently calculated with exact fractions across several phase carries.
  const std::vector<short> changing{-32768, 32767, -12345, 23456, -1, 0, 1, 32000,
                                    -32000, 123, 456, -789, 8192, -16384, 30000, -30000};
  require(audio::ResamplePcm(changing, 1, 44100, 48000) ==
              std::vector<short>{-32768, 27442, -5014, 14730, 7623, 0, 1, 13801,
                                 9600, -23367, 185, 324, -564, 7687, -13005, 19854,
                                 -12000, -30000},
          "fractional phase carry must preserve the authored waveform");
  require(audio::ResamplePcm(std::vector<short>{-2, 2}, 1, 1, 3) ==
              std::vector<short>{-2, -1, 1, 2, 2, 2},
          "odd denominators must round signed interior samples symmetrically");
  const int maximumRate = std::numeric_limits<int>::max();
  require(audio::ResamplePcm(std::vector<short>{-32768, 32767}, 1,
                            maximumRate - 1, maximumRate) ==
              std::vector<short>{-32768, 32767, 32767},
          "large rate numerators must not overflow signed sample arithmetic");
  require(audio::ResamplePcm(std::vector<short>{123, -456}, 1, maximumRate, 1) ==
              std::vector<short>{123},
          "a large whole-frame step must not advance beyond the source");
}
void benchmark(double budgetMilliseconds) {
  std::vector<short> input(44100 * 2 * 10);
  for (std::size_t i = 0; i < input.size(); ++i)
    input[i] = static_cast<short>(static_cast<int>((i * 17) % 65536) - 32768);
  const auto start = std::chrono::steady_clock::now();
  const auto output = audio::ResamplePcm(input, 2, 44100, 48000);
  const double milliseconds = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count();
  require(output.size() == 960000, "benchmark output length changed");
  std::cout << "Resample 10 s stereo 44.1 -> 48 kHz: " << milliseconds << " ms\n";
  require(milliseconds <= budgetMilliseconds, "device resampling budget exceeded");
}
}

int main(int argc, char **argv) {
  try {
    // Explicit device probe only; ordinary CTest has no wall-clock gate.
    if (argc > 1 && std::string(argv[1]) == "--benchmark") {
      benchmark(argc > 2 ? std::stod(argv[2]) : std::numeric_limits<double>::max());
    } else {
      checkSamples();
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
