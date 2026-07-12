#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace audio {
enum class PlaybackMode : std::uint8_t { PitchShift = 0, TimeStretch = 1 };

struct PlaybackRate {
  int percent = 100;
  PlaybackMode mode = PlaybackMode::PitchShift;

  [[nodiscard]] bool valid() const noexcept {
    return (mode == PlaybackMode::PitchShift ||
            mode == PlaybackMode::TimeStretch) &&
           percent >= 50 && percent <= 200 && percent % 5 == 0;
  }
  [[nodiscard]] bool neutral() const noexcept { return percent == 100; }

  [[nodiscard]] long long chartMicrosFromReal(long long value) const noexcept {
    return scaled(value, percent, 100);
  }

  [[nodiscard]] long long realMicrosFromChart(long long value) const noexcept {
    return percent <= 0 ? 0 : scaled(value, 100, percent);
  }

  bool operator==(const PlaybackRate &) const = default;

private:
  static long long scaled(long long value, long long numerator,
                          long long denominator) noexcept {
#if defined(__SIZEOF_INT128__)
    const __int128 result = static_cast<__int128>(value) * numerator /
                            static_cast<__int128>(denominator);
    return static_cast<long long>(std::clamp(
        result, static_cast<__int128>(std::numeric_limits<long long>::min()),
        static_cast<__int128>(std::numeric_limits<long long>::max())));
#else
    const long double result = static_cast<long double>(value) *
                               static_cast<long double>(numerator) /
                               static_cast<long double>(denominator);
    if (result >=
        static_cast<long double>(std::numeric_limits<long long>::max())) {
      return std::numeric_limits<long long>::max();
    }
    if (result <=
        static_cast<long double>(std::numeric_limits<long long>::min())) {
      return std::numeric_limits<long long>::min();
    }
    return static_cast<long long>(result);
#endif
  }
};
} // namespace audio
