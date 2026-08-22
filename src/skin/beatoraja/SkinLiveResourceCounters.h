#pragma once

#include <atomic>
#include <compare>
#include <cstdint>
#include <exception>
#include <limits>

namespace skin {

// Application-owned counts for catalog-published resource graphs and their
// unique physical texture handles.  They deliberately do not estimate
// process or GPU residency: those require a platform/process measurement.
struct SkinLiveResourceSnapshot {
  std::uint64_t liveTextures = 0;
  std::uint64_t liveResources = 0;
  std::uint64_t liveCpuPixmaps = 0;
  std::uint64_t liveCpuPixmapBytes = 0;
  std::uint64_t liveMovies = 0;
  std::uint64_t liveMovieBytes = 0;
  std::uint64_t liveAudioIdentities = 0;
  std::uint64_t liveAudioDecodedBytes = 0;

  auto operator<=>(const SkinLiveResourceSnapshot &) const = default;
};

class SkinLiveResourceCounters final {
public:
  void textureCreated() noexcept { increment(true); }
  void textureDestroyed() noexcept { decrement(true); }
  void resourceCreated() noexcept { increment(false); }
  void resourceDestroyed() noexcept { decrement(false); }
  void generatedPixmapCreated(std::uint64_t bytes) noexcept {
    add(cpuPixmapBytes_, bytes);
    add(cpuPixmaps_, 1);
  }
  void generatedPixmapDestroyed(std::uint64_t bytes) noexcept {
    subtract(cpuPixmapBytes_, bytes);
    subtract(cpuPixmaps_, 1);
  }
  void movieCreated(std::uint64_t bytes) noexcept {
    add(movieBytes_, bytes);
    add(movies_, 1);
  }
  void movieDestroyed(std::uint64_t bytes) noexcept {
    subtract(movieBytes_, bytes);
    subtract(movies_, 1);
  }
  void audioCreated(std::uint64_t decodedBytes) noexcept {
    add(audioDecodedBytes_, decodedBytes);
    add(audioIdentities_, 1);
  }
  void audioDestroyed(std::uint64_t decodedBytes) noexcept {
    subtract(audioDecodedBytes_, decodedBytes);
    subtract(audioIdentities_, 1);
  }

  [[nodiscard]] SkinLiveResourceSnapshot snapshot() const noexcept {
    // One atomic word makes this a coherent, allocation-free point-in-time
    // read without taking a lock on the main thread.
    const std::uint64_t packed = counts_.load(std::memory_order_acquire);
    return {.liveTextures = packed & kCounterMask,
            .liveResources = packed >> kCounterBits,
            .liveCpuPixmaps = cpuPixmaps_.load(std::memory_order_acquire),
            .liveCpuPixmapBytes =
                cpuPixmapBytes_.load(std::memory_order_acquire),
            .liveMovies = movies_.load(std::memory_order_acquire),
            .liveMovieBytes = movieBytes_.load(std::memory_order_acquire),
            .liveAudioIdentities =
                audioIdentities_.load(std::memory_order_acquire),
            .liveAudioDecodedBytes =
                audioDecodedBytes_.load(std::memory_order_acquire)};
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

  static void add(std::atomic_uint64_t &value,
                  std::uint64_t amount) noexcept {
    std::uint64_t expected = value.load(std::memory_order_relaxed);
    for (;;) {
      if (amount > std::numeric_limits<std::uint64_t>::max() - expected) {
        std::terminate();
      }
      if (value.compare_exchange_weak(expected, expected + amount,
                                      std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
        return;
      }
    }
  }

  static void subtract(std::atomic_uint64_t &value,
                       std::uint64_t amount) noexcept {
    std::uint64_t expected = value.load(std::memory_order_relaxed);
    for (;;) {
      if (amount > expected) {
        std::terminate();
      }
      if (value.compare_exchange_weak(expected, expected - amount,
                                      std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
        return;
      }
    }
  }

  std::atomic_uint64_t counts_{0};
  std::atomic_uint64_t cpuPixmaps_{0};
  std::atomic_uint64_t cpuPixmapBytes_{0};
  std::atomic_uint64_t movies_{0};
  std::atomic_uint64_t movieBytes_{0};
  std::atomic_uint64_t audioIdentities_{0};
  std::atomic_uint64_t audioDecodedBytes_{0};
};

} // namespace skin
