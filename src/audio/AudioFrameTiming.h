#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace audio::frame_timing {

// One chart-zero output grid for starts, controls, cuts and seeks. Quantize
// each absolute position once; converting through integer real microseconds,
// or rounding a time difference, can move an event to the adjacent frame.
inline std::int64_t at(long long micros, int sampleRate, int ratePercent) noexcept {
  const auto rate = ratePercent > 0 ? ratePercent : 100;
#if defined(__SIZEOF_INT128__)
  const __int128 denominator = static_cast<__int128>(1000000) * rate;
  const __int128 numerator = static_cast<__int128>(micros) * sampleRate * 100 + denominator / 2;
  const __int128 frame = numerator / denominator - (numerator < 0 && numerator % denominator != 0);
  return static_cast<std::int64_t>(std::clamp(frame,
      static_cast<__int128>(std::numeric_limits<std::int64_t>::min()),
      static_cast<__int128>(std::numeric_limits<std::int64_t>::max())));
#else
  const long double frame = std::floor(static_cast<long double>(micros) * sampleRate * 100 /
                                      (1000000.0L * rate) + 0.5L);
  if (frame >= std::numeric_limits<std::int64_t>::max()) return std::numeric_limits<std::int64_t>::max();
  if (frame <= std::numeric_limits<std::int64_t>::min()) return std::numeric_limits<std::int64_t>::min();
  return static_cast<std::int64_t>(frame);
#endif
}

inline std::int64_t difference(std::int64_t frame, std::int64_t origin) noexcept {
  if (origin < 0 && frame > std::numeric_limits<std::int64_t>::max() + origin)
    return std::numeric_limits<std::int64_t>::max();
  if (origin > 0 && frame < std::numeric_limits<std::int64_t>::min() + origin)
    return std::numeric_limits<std::int64_t>::min();
  return frame - origin;
}

inline constexpr std::uint64_t one = std::uint64_t{1} << 32;
inline std::uint64_t increment(int ratePercent) noexcept {
  return (static_cast<std::uint64_t>(ratePercent > 0 ? ratePercent : 100) * one + 50) / 100;
}

inline std::uint64_t positionAfter(std::uint64_t frames, int ratePercent) noexcept {
  const auto step = increment(ratePercent);
  return frames > std::numeric_limits<std::uint64_t>::max() / step
      ? std::numeric_limits<std::uint64_t>::max() : frames * step;
}

inline std::uint64_t restoredPosition(long long triggerMicros, long long originMicros,
                                      int sampleRate, int ratePercent) noexcept {
  const auto elapsed = difference(at(originMicros, sampleRate, ratePercent),
                                   at(triggerMicros, sampleRate, ratePercent));
  return positionAfter(std::max<std::int64_t>(0, elapsed), ratePercent);
}

inline std::uint64_t outputFrames(std::uint32_t preparedFrames, int ratePercent) noexcept {
  const auto length = static_cast<std::uint64_t>(preparedFrames) << 32;
  const auto step = increment(ratePercent);
  return length / step + (length % step != 0);
}

} // namespace audio::frame_timing
