#pragma once

// Opt-in benchmark instrumentation. Normal builds evaluate only the operation:
// no clocks, counters, thread-local state, or branches in the audio loader.
#if defined(ASOBMASHOW_AUDIO_DECODE_BENCHMARK) || defined(ASOBMASHOW_AUDIO_DECODE_PROFILE)
#include <chrono>
#include <cstdint>
#include <utility>

namespace audio::decode_profiling {
struct Timings {
  std::uint64_t input = 0;
  std::uint64_t open = 0;
  std::uint64_t read = 0;
  std::uint64_t convert = 0;
};
inline thread_local Timings timings;
template <typename Operation>
auto measure(std::uint64_t Timings::*stage, Operation &&operation) {
  const auto start = std::chrono::steady_clock::now();
  struct Finish {
    std::uint64_t Timings::*stage;
    std::chrono::steady_clock::time_point start;
    ~Finish() {
      timings.*stage += std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start).count();
    }
  } finish{stage, start};
  return std::forward<Operation>(operation)();
}
} // namespace audio::decode_profiling
#endif

#ifdef ASOBMASHOW_AUDIO_DECODE_PROFILE
#define ASOBM_AUDIO_MEASURE(stage, ...) \
  ::audio::decode_profiling::measure(&::audio::decode_profiling::Timings::stage, [&] { return __VA_ARGS__; })
#else
#define ASOBM_AUDIO_MEASURE(stage, ...) (__VA_ARGS__)
#endif
