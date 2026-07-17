#include "input/InputTimestamp.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void testRebasesPastNativeTimestampIntoSteadyClockDomain() {
  require(input::rebaseTimestampMicros(7'500, 10'000, 1'000'000) ==
              997'500,
          "native event age is preserved in the steady-clock domain");
}

void testRebasesFutureNativeTimestampIntoSteadyClockDomain() {
  require(input::rebaseTimestampMicros(10'125, 10'000, 1'000'000) ==
              1'000'125,
          "a slightly future native sample preserves its signed offset");
}

void testRebaseSaturatesInsteadOfOverflowing() {
  require(input::rebaseTimestampMicros(
              std::numeric_limits<std::uint64_t>::max(), 0,
              std::numeric_limits<std::int64_t>::max() - 5) ==
              std::numeric_limits<std::int64_t>::max(),
          "timestamp rebasing saturates at the signed clock limit");
}

void testRebasesWrappingSdlMillisecondTimestamps() {
  require(input::rebaseWrappingTimestampMillis(9'997, 10'000, 1'000'000) ==
              997'000,
          "SDL event age is preserved in the steady-clock domain");
  require(input::rebaseWrappingTimestampMillis(2, 1, 1'000'000) ==
              1'001'000,
          "a slightly future SDL timestamp preserves its signed offset");
  require(input::rebaseWrappingTimestampMillis(
              std::numeric_limits<std::uint32_t>::max() - 1, 1,
              1'000'000) == 997'000,
          "SDL's 32-bit millisecond wrap preserves a recent event's age");
}

} // namespace

int main() {
  testRebasesPastNativeTimestampIntoSteadyClockDomain();
  testRebasesFutureNativeTimestampIntoSteadyClockDomain();
  testRebaseSaturatesInsteadOfOverflowing();
  testRebasesWrappingSdlMillisecondTimestamps();
  return 0;
}
