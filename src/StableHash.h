#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace stable_hash {

// Persisted device IDs and cache names depend on these exact byte/format rules.
[[nodiscard]] inline std::uint64_t fnv1a64(std::string_view value) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

[[nodiscard]] inline std::string hex64(std::uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result(16, '0');
  for (std::size_t index = result.size(); index > 0; --index) {
    result[index - 1] = digits[value & 0xFU];
    value >>= 4U;
  }
  return result;
}

} // namespace stable_hash
