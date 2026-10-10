#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

#ifndef ASOBMASHOW_ENABLE_PERF_TELEMETRY
#define ASOBMASHOW_ENABLE_PERF_TELEMETRY 0
#endif

namespace perf::latency {

enum class Stage : unsigned {
  InputDelivery, IngressToWorker, WorkerToSoundCommit, SoundCommandToCallback,
  CallbackDuration, CallbackInterval, NativeOutputLead, NativeOutputLateness,
  SnapshotAge, FrameSubmit, TouchToWorker, TouchToSoundCommit, Count
};
inline constexpr std::array names{
    "source-to-ingress", "ingress-to-worker", "worker-to-sound-command",
    "sound-command-to-callback", "audio-callback", "audio-interval",
    "native-output-lead", "native-output-lateness", "snapshot-age", "frame-submit",
    "touch-to-worker", "touch-to-sound-command"};

inline std::int64_t nowMicros() noexcept {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Summary {
  std::uint64_t count = 0, p50 = 0, p95 = 0, p99 = 0, maximum = 0;
};

// Fixed storage, lock-free updates, no allocations or logging on realtime paths.
// Percentiles are bucket upper bounds (100 us resolution through 25.5 ms).
// The overflow bucket conservatively reports the observed maximum. Cumulative
// snapshots are approximate while writers run; they are not an E2E measurement.
class Histogram {
public:
  void record(std::uint64_t micros) noexcept {
    auto maximum = maximum_.load(std::memory_order_relaxed);
    while (micros > maximum && !maximum_.compare_exchange_weak(
               maximum, micros, std::memory_order_relaxed)) {}
    buckets_[std::min<std::uint64_t>(micros / 100 + (micros % 100 != 0), 256)].fetch_add(
        1, std::memory_order_release);
  }

  Summary snapshot() const noexcept {
    std::array<std::uint64_t, 257> counts{};
    Summary result;
    for (unsigned i = 0; i < counts.size(); ++i) {
      counts[i] = buckets_[i].load(std::memory_order_acquire);
      result.count += counts[i];
    }
    result.maximum = maximum_.load(std::memory_order_relaxed);
    if (!result.count) return result;
    auto percentile = [&](std::uint64_t percent) {
      const auto target = (result.count * percent + 99) / 100;
      std::uint64_t count = 0;
      for (unsigned i = 0; i < counts.size(); ++i) {
        count += counts[i];
        if (count >= target) return i == 256 ? result.maximum : static_cast<std::uint64_t>(i) * 100;
      }
      return result.maximum;
    };
    result.p50 = percentile(50);
    result.p95 = percentile(95);
    result.p99 = percentile(99);
    return result;
  }

private:
  std::array<std::atomic<std::uint64_t>, 257> buckets_{};
  std::atomic<std::uint64_t> maximum_{0};
};

inline std::array<Histogram, static_cast<unsigned>(Stage::Count)> histograms;
inline void record(Stage stage, std::uint64_t micros) noexcept {
#if ASOBMASHOW_ENABLE_PERF_TELEMETRY
  histograms[static_cast<unsigned>(stage)].record(micros);
#else
  (void)stage;
  (void)micros;
#endif
}
inline Summary snapshot(Stage stage) noexcept {
  return histograms[static_cast<unsigned>(stage)].snapshot();
}

} // namespace perf::latency
