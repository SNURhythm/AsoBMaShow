#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

namespace replay_duration {

[[nodiscard]] inline std::uint64_t
ceilNonnegativeMicrosToMilliseconds(std::int64_t micros) noexcept {
  const auto nonnegative = std::max<std::int64_t>(0, micros);
  return static_cast<std::uint64_t>(nonnegative / 1000) +
         static_cast<std::uint64_t>(nonnegative % 1000 != 0);
}

[[nodiscard]] inline std::optional<std::int64_t>
addNonnegativeMicros(std::int64_t left, std::int64_t right) noexcept {
  if (left < 0 || right < 0) {
    return std::nullopt;
  }
  if (right > std::numeric_limits<std::int64_t>::max() - left) {
    return std::nullopt;
  }
  return left + right;
}

} // namespace replay_duration
