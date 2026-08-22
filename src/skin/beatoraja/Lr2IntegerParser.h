#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace skin {

static_assert(std::numeric_limits<int>::digits == 31);

// Integer.parseInt-compatible ASCII boundary used by LR2 command fields.
// Unlike condition expressions, command integers receive no trimming or
// character filtering before the pinned loader parses them.
[[nodiscard]] inline std::optional<int>
parseLr2JavaInteger(std::string_view value) noexcept {
  if (value.empty()) return std::nullopt;
  std::size_t index = 0;
  bool negative = false;
  if (value.front() == '+' || value.front() == '-') {
    negative = value.front() == '-';
    if (++index == value.size()) return std::nullopt;
  }
  constexpr std::uint32_t positiveLimit = 2'147'483'647U;
  constexpr std::uint32_t negativeLimit = 2'147'483'648U;
  const std::uint32_t limit = negative ? negativeLimit : positiveLimit;
  std::uint32_t magnitude = 0;
  for (; index < value.size(); ++index) {
    const char character = value[index];
    if (character < '0' || character > '9') return std::nullopt;
    const std::uint32_t digit = static_cast<std::uint32_t>(character - '0');
    if (magnitude > (limit - digit) / 10U) return std::nullopt;
    magnitude = magnitude * 10U + digit;
  }
  if (!negative) return static_cast<int>(magnitude);
  if (magnitude == negativeLimit) return std::numeric_limits<int>::min();
  return -static_cast<int>(magnitude);
}

} // namespace skin
