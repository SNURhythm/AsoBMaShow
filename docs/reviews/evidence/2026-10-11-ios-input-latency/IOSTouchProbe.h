#pragma once
#include "LatencyTelemetry.h"
#include <SDL3/SDL_log.h>
#include <array>
#include <atomic>

namespace ios_touch_probe {
enum class Stage : unsigned { SourceUIKit, UIKitGameplay, GameplayEnqueue,
    UIKitEnqueue, SourceEnqueue, HandlerDuration, Count };
inline constexpr std::array names{"source-to-uikit", "uikit-to-gameplay",
    "gameplay-to-enqueue", "uikit-to-enqueue", "source-to-enqueue", "uikit-handler"};
class Histogram {
  std::array<std::atomic<std::uint64_t>, 16385> buckets_{};
  std::atomic<std::uint64_t> maximum_{0};
public:
  void record(std::uint64_t us) {
    auto max = maximum_.load(std::memory_order_relaxed);
    while (us > max && !maximum_.compare_exchange_weak(max, us, std::memory_order_relaxed)) {}
    buckets_[std::min<std::uint64_t>(us, 16384)].fetch_add(1, std::memory_order_relaxed);
  }
  perf::latency::Summary snapshot() const {
    std::array<std::uint64_t, 16385> counts{};
    perf::latency::Summary result;
    for (unsigned i = 0; i < counts.size(); ++i) {
      counts[i] = buckets_[i].load(std::memory_order_relaxed);
      result.count += counts[i];
    }
    result.maximum = maximum_.load(std::memory_order_relaxed);
    const auto percentile = [&](std::uint64_t p) -> std::uint64_t {
      if (result.count == 0) return 0;
      const auto target = (result.count * p + 99) / 100;
      std::uint64_t count = 0;
      for (unsigned i = 0; i < counts.size(); ++i) {
        count += counts[i];
        if (count >= target) return i == 16384 ? result.maximum : i;
      }
      return result.maximum;
    };
    result.p50 = percentile(50); result.p95 = percentile(95); result.p99 = percentile(99);
    return result;
  }
};
inline std::array<Histogram, static_cast<unsigned>(Stage::Count)> histograms;
inline std::atomic<std::uint64_t> matched{0}, missing{0}, sdlFingers{0};
inline std::atomic<std::int64_t> lastEnqueue{0};
struct Handler;
inline thread_local Handler *activeHandler = nullptr;
struct Handler {
  Handler *previous = activeHandler;
  std::int64_t entered = perf::latency::nowMicros();
  std::int64_t gameplay = 0, source = 0;
  bool enqueued = false;
  Handler() { activeHandler = this; }
  ~Handler() {
    const auto finished = perf::latency::nowMicros();
    if (enqueued) histograms[static_cast<unsigned>(Stage::HandlerDuration)].record(finished - entered);
    activeHandler = previous;
  }
};
inline void markGameplay(std::int64_t source) {
  if (!activeHandler) return;
  activeHandler->gameplay = perf::latency::nowMicros();
  activeHandler->source = source;
}
inline void markEnqueue(std::int64_t source, std::int64_t now) {
  auto *handler = activeHandler;
  if (!handler || handler->source != source || !handler->gameplay) {
    missing.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  lastEnqueue.store(now, std::memory_order_relaxed);
  handler->enqueued = true;
  matched.fetch_add(1, std::memory_order_relaxed);
  const auto record = [](Stage stage, std::int64_t value) {
    if (value >= 0) histograms[static_cast<unsigned>(stage)].record(value);
  };
  record(Stage::SourceUIKit, handler->entered - source);
  record(Stage::UIKitGameplay, handler->gameplay - handler->entered);
  record(Stage::GameplayEnqueue, now - handler->gameplay);
  record(Stage::UIKitEnqueue, now - handler->entered);
  record(Stage::SourceEnqueue, now - source);
}
inline void dump() {
  // Snapshot only while input is idle; scanning fine histograms must not delay a tap.
  const auto last = lastEnqueue.load(std::memory_order_relaxed);
  if (!last || perf::latency::nowMicros() - last < 1000000) return;
  SDL_Log("iOS touch probe matched %llu missing %llu synchronous SDL fingers %llu",
      (unsigned long long)matched.load(), (unsigned long long)missing.load(),
      (unsigned long long)sdlFingers.load());
  for (unsigned i = 0; i < static_cast<unsigned>(Stage::Count); ++i) {
    const auto s = histograms[i].snapshot();
    if (!s.count) continue;
    SDL_Log("iOS touch segment %s | n %llu | p50/p95/p99 <= %llu/%llu/%llu us | max %llu us",
        names[i], (unsigned long long)s.count, (unsigned long long)s.p50,
        (unsigned long long)s.p95, (unsigned long long)s.p99, (unsigned long long)s.maximum);
  }
}
} // namespace ios_touch_probe
void InstallIOSTouchProbe();
