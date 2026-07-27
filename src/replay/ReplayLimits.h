#pragma once

#include "../DurablePayloadLimits.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace replay {

struct ReplayLimits {
  std::size_t maxCompressedBytes = 64U * 1024U * 1024U;
  std::size_t maxJsonBytes = 256U * 1024U * 1024U;
  std::size_t maxKeyInputBytes = 9U * 1'000'000U;
  std::size_t maxInputTransitions = 1'000'000U;
  std::size_t maxTouchSamples = 1'000'000U;
  std::size_t maxLaneCoverEvents = 100'000U;
  std::size_t maxRandomValues = 100'000U;
  std::size_t maxCourseStages = durable_payload::kMaximumCourseStages;
  std::size_t maxJsonDepth = 64U;
  std::size_t maxStringBytes = durable_payload::kMaximumStringBytes;
  std::size_t maxFilenameBytes = 255U;
  std::int64_t minimumSongTimeMicros = -30'000'000LL;
  std::int64_t maxCourseRestMicros = 60LL * 60LL * 1'000'000LL;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return maxCompressedBytes > 0 && maxJsonBytes > 0 &&
           maxCompressedBytes <= maxJsonBytes && maxKeyInputBytes > 0 &&
           maxKeyInputBytes <= maxJsonBytes && maxInputTransitions > 0 &&
           maxTouchSamples > 0 && maxLaneCoverEvents > 0 &&
           maxRandomValues > 0 && maxCourseStages > 0 && maxJsonDepth > 0 &&
           maxStringBytes > 0 && maxFilenameBytes > 0 &&
           minimumSongTimeMicros <= 0 && maxCourseRestMicros >= 0;
  }

  bool operator==(const ReplayLimits &) const = default;
};

inline constexpr ReplayLimits kReplayLimits{};
static_assert(kReplayLimits.valid());

[[nodiscard]] constexpr bool
withinReplayCountLimit(std::size_t count, std::size_t maximum) noexcept {
  return count <= maximum;
}

struct ReplayTimeBounds {
  std::int64_t completionSongTimeMicros = -1;

  bool operator==(const ReplayTimeBounds &) const = default;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return completionSongTimeMicros >= 0;
  }

  [[nodiscard]] constexpr bool
  contains(std::int64_t songTimeMicros,
           const ReplayLimits &limits = kReplayLimits) const noexcept {
    return limits.valid() && valid() &&
           songTimeMicros >= limits.minimumSongTimeMicros &&
           songTimeMicros <= completionSongTimeMicros;
  }
};

[[nodiscard]] constexpr bool
isMonotonicReplayTime(std::int64_t previousSongTimeMicros,
                      std::int64_t nextSongTimeMicros, ReplayTimeBounds bounds,
                      const ReplayLimits &limits = kReplayLimits) noexcept {
  return nextSongTimeMicros >= previousSongTimeMicros &&
         bounds.contains(nextSongTimeMicros, limits);
}

[[nodiscard]] constexpr bool
validCourseRestMicros(std::int64_t restMicros,
                      const ReplayLimits &limits = kReplayLimits) noexcept {
  return limits.valid() && restMicros >= 0 &&
         restMicros <= limits.maxCourseRestMicros;
}

[[nodiscard]] constexpr std::int64_t
clampCourseRestMicros(std::int64_t restMicros,
                      const ReplayLimits &limits = kReplayLimits) noexcept {
  if (!limits.valid()) {
    return 0;
  }
  return std::clamp(restMicros, std::int64_t{0}, limits.maxCourseRestMicros);
}

} // namespace replay
