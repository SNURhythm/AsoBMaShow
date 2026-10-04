#pragma once

#include "bms_parser.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

// Parser timings are integer microseconds. Never round-trip a stored STOP
// through double: LLONG_MAX rounds up to the unrepresentable value 2^63.
namespace chart_timing {
inline long long rounded(double value) noexcept {
  if (std::isnan(value)) return 0;
  if (value >= static_cast<double>(std::numeric_limits<long long>::max()))
    return std::numeric_limits<long long>::max();
  if (value <= static_cast<double>(std::numeric_limits<long long>::min()))
    return std::numeric_limits<long long>::min();
  return std::llround(value);
}

inline long long add(long long left, long long right) noexcept {
  if (right > 0 && left > std::numeric_limits<long long>::max() - right)
    return std::numeric_limits<long long>::max();
  if (right < 0 && left < std::numeric_limits<long long>::min() - right)
    return std::numeric_limits<long long>::min();
  return left + right;
}

inline long long subtract(long long left, long long right) noexcept {
  if (right > 0 && left < std::numeric_limits<long long>::min() + right)
    return std::numeric_limits<long long>::min();
  if (right < 0 && left > std::numeric_limits<long long>::max() + right)
    return std::numeric_limits<long long>::max();
  return left - right;
}

inline long long stopDuration(const bms_parser::TimeLine &timeline) noexcept {
  return std::max(0LL, timeline.ParsedStopDuration.value_or(
                           rounded(timeline.GetStopDuration())));
}

inline long long stopEnd(const bms_parser::TimeLine &timeline) noexcept {
  return add(timeline.Timing, stopDuration(timeline));
}
} // namespace chart_timing
