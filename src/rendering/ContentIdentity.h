#pragma once

#include <atomic>
#include <cstdint>

namespace rendering {
// Process-wide identities never depend on reusable addresses, bgfx handles,
// authored resource IDs, decoder timestamps, or presentation frame serials.
// Zero means that the producer cannot promise stable, complete content.
inline std::uint64_t nextContentIdentity() noexcept {
  static std::atomic<std::uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}
} // namespace rendering
