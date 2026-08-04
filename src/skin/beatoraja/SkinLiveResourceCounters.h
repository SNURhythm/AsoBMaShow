#pragma once

#include <atomic>
#include <compare>
#include <cstdint>
#include <exception>

namespace skin {

// Application-owned counts for catalog-published resource graphs and their
// unique physical texture handles.  They deliberately do not estimate
// process or GPU residency: those require a platform/process measurement.
struct SkinLiveResourceSnapshot {
  std::uint64_t liveTextures = 0;
  std::uint64_t liveResources = 0;

  auto operator<=>(const SkinLiveResourceSnapshot &) const = default;
};

class SkinLiveResourceCounters final {
public:
  void textureCreated() noexcept { increment(true); }
  void textureDestroyed() noexcept { decrement(true); }
  void resourceCreated() noexcept { increment(false); }
  void resourceDestroyed() noexcept { decrement(false); }

  [[nodiscard]] SkinLiveResourceSnapshot snapshot() const noexcept {
    // One atomic word makes this a coherent, allocation-free point-in-time
    // read without taking a lock on the main thread.
    const std::uint64_t packed = counts_.load(std::memory_order_acquire);
    return {.liveTextures = packed & kCounterMask,
            .liveResources = packed >> kCounterBits};
  }

private:
  static constexpr std::uint64_t kCounterBits = 32;
  static constexpr std::uint64_t kCounterMask =
      (std::uint64_t{1} << kCounterBits) - 1;

  static std::uint64_t textureCount(std::uint64_t packed) noexcept {
    return packed & kCounterMask;
  }

  static std::uint64_t resourceCount(std::uint64_t packed) noexcept {
    return packed >> kCounterBits;
  }

  void increment(bool texture) noexcept { mutate(texture, true); }
  void decrement(bool texture) noexcept { mutate(texture, false); }

  void mutate(bool texture, bool incrementing) noexcept {
    std::uint64_t expected = counts_.load(std::memory_order_relaxed);
    for (;;) {
      const std::uint64_t selected =
          texture ? textureCount(expected) : resourceCount(expected);
      if ((!incrementing && selected == 0) ||
          (incrementing && selected == kCounterMask)) {
        std::terminate();
      }
      const std::uint64_t replacement =
          texture
              ? ((expected & ~kCounterMask) |
                 (incrementing ? selected + 1 : selected - 1))
              : ((expected & kCounterMask) |
                 ((incrementing ? selected + 1 : selected - 1)
                  << kCounterBits));
      if (counts_.compare_exchange_weak(expected, replacement,
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
        return;
      }
    }
  }

  std::atomic_uint64_t counts_{0};
};

} // namespace skin
