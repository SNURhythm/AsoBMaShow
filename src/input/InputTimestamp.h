#pragma once

#include <cstdint>
#include <limits>

namespace input {
namespace detail {

inline constexpr std::uint64_t kSignedNegativeMagnitude =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;

constexpr std::uint64_t negativeMagnitude(std::int64_t value) noexcept {
  return static_cast<std::uint64_t>(-(value + 1)) + 1U;
}

constexpr std::int64_t fromNegativeMagnitude(
    std::uint64_t magnitude) noexcept {
  if (magnitude == 0) {
    return 0;
  }
  if (magnitude >= kSignedNegativeMagnitude) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return -static_cast<std::int64_t>(magnitude);
}

constexpr std::int64_t saturatingAdd(std::int64_t value,
                                     std::uint64_t delta) noexcept {
  const auto maximum =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (value >= 0) {
    const auto positive = static_cast<std::uint64_t>(value);
    return delta > maximum - positive
               ? std::numeric_limits<std::int64_t>::max()
               : static_cast<std::int64_t>(positive + delta);
  }

  const auto magnitude = negativeMagnitude(value);
  const auto available = maximum + magnitude;
  if (delta > available) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (delta < magnitude) {
    return fromNegativeMagnitude(magnitude - delta);
  }
  return static_cast<std::int64_t>(delta - magnitude);
}

constexpr std::int64_t saturatingSubtract(std::int64_t value,
                                          std::uint64_t delta) noexcept {
  if (value <= 0) {
    const auto magnitude = negativeMagnitude(value);
    const auto available = kSignedNegativeMagnitude - magnitude;
    return delta > available
               ? std::numeric_limits<std::int64_t>::min()
               : fromNegativeMagnitude(magnitude + delta);
  }

  const auto positive = static_cast<std::uint64_t>(value);
  const auto available = kSignedNegativeMagnitude + positive;
  if (delta > available) {
    return std::numeric_limits<std::int64_t>::min();
  }
  if (delta <= positive) {
    return static_cast<std::int64_t>(positive - delta);
  }
  return fromNegativeMagnitude(delta - positive);
}

} // namespace detail

// Preserves the sample's age while moving it from a native monotonic clock
// epoch into the process steady-clock epoch used by the audio clock anchor.
constexpr std::int64_t rebaseTimestampMicros(
    std::uint64_t sourceTimestampMicros, std::uint64_t sourceNowMicros,
    std::int64_t steadyNowMicros) noexcept {
  return sourceTimestampMicros <= sourceNowMicros
             ? detail::saturatingSubtract(
                   steadyNowMicros, sourceNowMicros - sourceTimestampMicros)
             : detail::saturatingAdd(
                   steadyNowMicros, sourceTimestampMicros - sourceNowMicros);
}

} // namespace input
