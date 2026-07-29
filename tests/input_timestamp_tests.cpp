#include "input/InputTimestamp.h"
#include "input/InputLifecycle.h"
#if defined(__APPLE__)
#include "input/AppleInputTimestamp.h"
#endif

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

void testFixedEpochMappingKeepsEqualNativeSamplesEqual() {
  constexpr input::TimestampEpochMapping mapping{
      .sourceEpochMicros = 10'000,
      .steadyEpochMicros = 1'000'000,
  };

  require(mapping.toSteadyMicros(7'500) == 997'500 &&
              mapping.toSteadyMicros(7'501) == 997'501,
          "one native timestamp has one stable steady-clock value");
}

#if defined(__APPLE__)
void testAppleHostTimestampConversionIsScopedToInputSession() {
  input::apple::HostToSteadyTimestampSession first(
      {.sourceEpochMicros = 10'000, .steadyEpochMicros = 1'000'000});
  const auto expected = first.toSteadyMicros(7'500);
  for (int sample = 0; sample < 10'000; ++sample) {
    require(first.toSteadyMicros(7'500) == expected,
            "equal Apple host timestamps remain equal within one input "
            "session");
  }

  first.reanchor(
      {.sourceEpochMicros = 10'000, .steadyEpochMicros = 2'000'000});
  require(first.toSteadyMicros(7'500) == 1'997'500 &&
              first.toSteadyMicros(7'500) != expected,
          "a resumed input session refreshes a changed native clock epoch "
          "instead of retaining process-lifetime skew");
}
#endif

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

void testForegroundLifecycleEventsShareOneInputPolicy() {
  SDL_Event event{};
  event.type = SDL_APP_DIDENTERFOREGROUND;
  require(input::isForegroundLifecycleEvent(event),
          "app foreground reanchors native input clocks");

  event.type = SDL_WINDOWEVENT;
  event.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
  require(input::isForegroundLifecycleEvent(event),
          "desktop focus recovery uses the same input lifecycle policy");

  event.type = SDL_APP_DIDENTERBACKGROUND;
  require(!input::isForegroundLifecycleEvent(event) &&
              input::isBackgroundLifecycleEvent(event),
          "background lifecycle never masquerades as a timestamp reanchor");
}

} // namespace

int main() {
  testRebasesPastNativeTimestampIntoSteadyClockDomain();
  testRebasesFutureNativeTimestampIntoSteadyClockDomain();
  testRebaseSaturatesInsteadOfOverflowing();
  testFixedEpochMappingKeepsEqualNativeSamplesEqual();
#if defined(__APPLE__)
  testAppleHostTimestampConversionIsScopedToInputSession();
#endif
  testRebasesWrappingSdlMillisecondTimestamps();
  testForegroundLifecycleEventsShareOneInputPolicy();
  return 0;
}
