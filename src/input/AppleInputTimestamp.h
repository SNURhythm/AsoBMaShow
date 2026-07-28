#pragma once

#if defined(__APPLE__)

#include "InputTimestamp.h"

#include <chrono>
#include <cstdint>
#include <mach/mach_time.h>

namespace input::apple {

inline mach_timebase_info_data_t hostTimebase() noexcept {
  mach_timebase_info_data_t value{};
  if (mach_timebase_info(&value) != KERN_SUCCESS || value.denom == 0) {
    value.numer = 1;
    value.denom = 1;
  }
  return value;
}

inline std::uint64_t hostTicksToMicros(std::uint64_t ticks) noexcept {
  static const mach_timebase_info_data_t timebase = hostTimebase();
  const auto nanoseconds = static_cast<unsigned __int128>(ticks) *
                           timebase.numer / timebase.denom;
  return static_cast<std::uint64_t>(nanoseconds / 1000U);
}

inline std::uint64_t hostNowMicros() noexcept {
  return hostTicksToMicros(mach_absolute_time());
}

inline std::int64_t steadyNowMicros() noexcept {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline TimestampEpochMapping hostToSteadyEpochMapping() noexcept {
  const std::int64_t steadyBefore = steadyNowMicros();
  const std::uint64_t hostNow = hostNowMicros();
  const std::int64_t steadyAfter = steadyNowMicros();
  return {
      .sourceEpochMicros = hostNow,
      .steadyEpochMicros =
          steadyBefore + (steadyAfter - steadyBefore) / 2,
  };
}

inline std::int64_t
steadyMicrosFromHostMicros(std::uint64_t hostTimestampMicros) noexcept {
  static const TimestampEpochMapping mapping = hostToSteadyEpochMapping();
  return mapping.toSteadyMicros(hostTimestampMicros);
}

} // namespace input::apple

#endif
