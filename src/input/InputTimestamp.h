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

// One native clock epoch must map to one process steady-clock epoch for the
// lifetime of an input session. Re-sampling both clocks for every event can
// make equal native timestamps move by a few microseconds, which in turn can
// make simultaneous input appear out of order.
struct TimestampEpochMapping {
  std::uint64_t sourceEpochMicros = 0;
  std::int64_t steadyEpochMicros = 0;

  [[nodiscard]] constexpr std::int64_t
  toSteadyMicros(std::uint64_t sourceTimestampMicros) const noexcept {
    return rebaseTimestampMicros(sourceTimestampMicros, sourceEpochMicros,
                                 steadyEpochMicros);
  }
};

// SDL event timestamps are 32-bit milliseconds and wrap. Treat differences
// within half the counter range as signed offsets from the current SDL tick,
// then move that offset into the process steady-clock epoch.
constexpr std::int64_t rebaseWrappingTimestampMillis(
    std::uint32_t sourceTimestampMillis, std::uint32_t sourceNowMillis,
    std::int64_t steadyNowMicros) noexcept {
  constexpr std::uint64_t kCounterRange = std::uint64_t{1} << 32U;
  constexpr std::uint64_t kHalfCounterRange = kCounterRange / 2U;
  const std::uint64_t forwardMillis = static_cast<std::uint32_t>(
      sourceTimestampMillis - sourceNowMillis);
  if (forwardMillis < kHalfCounterRange) {
    return detail::saturatingAdd(steadyNowMicros, forwardMillis * 1000U);
  }
  return detail::saturatingSubtract(
      steadyNowMicros, (kCounterRange - forwardMillis) * 1000U);
}

} // namespace input
