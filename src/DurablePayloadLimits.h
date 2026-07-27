#pragma once

#include <cstddef>
#include <string_view>

namespace durable_payload {

inline constexpr std::size_t kMaximumStringBytes = 16U * 1024U;
inline constexpr std::size_t kMaximumResultGaugeSamples = 1'000'000U;
inline constexpr std::size_t kMaximumCourseStages = 256U;
inline constexpr std::size_t kMaximumIrSnapshotBytes = 16U * 1024U * 1024U;

[[nodiscard]] inline constexpr bool
withinLimit(std::size_t count, std::size_t maximum) noexcept {
  return count <= maximum;
}

[[nodiscard]] inline constexpr bool
validString(std::string_view value, bool allowEmpty) noexcept {
  return (allowEmpty || !value.empty()) && value.size() <= kMaximumStringBytes;
}

} // namespace durable_payload
