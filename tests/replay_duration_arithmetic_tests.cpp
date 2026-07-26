#include "ReplayDurationArithmetic.h"

#include <cstdint>
#include <iostream>
#include <limits>

int main() {
  constexpr std::uint64_t expected = 9'223'372'036'854'776ULL;
  const auto actual = replay_duration::ceilNonnegativeMicrosToMilliseconds(
      std::numeric_limits<std::int64_t>::max());
  if (actual != expected) {
    std::cerr << "FAIL: INT64_MAX microseconds rounds up without overflow\n";
    return 1;
  }
  const auto exact = replay_duration::addNonnegativeMicros(
      std::numeric_limits<std::int64_t>::max() - 5, 5);
  if (exact != std::numeric_limits<std::int64_t>::max()) {
    std::cerr << "FAIL: an in-range duration sum reaches INT64_MAX\n";
    return 1;
  }
  if (replay_duration::addNonnegativeMicros(
          std::numeric_limits<std::int64_t>::max(), 1)) {
    std::cerr << "FAIL: an overflowing duration sum is rejected\n";
    return 1;
  }
  std::cout << "Replay duration arithmetic tests passed\n";
  return 0;
}
