#include "SkinOverlayDigestProvider.h"

#include <atomic>
#include <limits>

namespace skin {

SkinOverlayDigestTicket nextSkinOverlayDigestTicket() noexcept {
  static std::atomic<std::uint64_t> next{1};
  auto candidate = next.load(std::memory_order_relaxed);
  while (candidate != std::numeric_limits<std::uint64_t>::max()) {
    if (next.compare_exchange_weak(candidate, candidate + 1,
                                   std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
      return {candidate};
    }
  }
  return {};
}

} // namespace skin
