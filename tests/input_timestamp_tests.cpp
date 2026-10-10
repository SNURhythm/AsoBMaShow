#include "input/InputTimestamp.h"
#include "input/AndroidInputTimestamp.h"
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

void testForegroundLifecycleEventsShareOneInputPolicy() {
  SDL_Event event{};
  event.type = SDL_EVENT_DID_ENTER_FOREGROUND;
  require(input::isForegroundLifecycleEvent(event),
          "app foreground reanchors native input clocks");

  event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
  require(input::isForegroundLifecycleEvent(event),
          "desktop focus recovery uses the same input lifecycle policy");

  event.type = SDL_EVENT_DID_ENTER_BACKGROUND;
  require(!input::isForegroundLifecycleEvent(event) &&
              input::isBackgroundLifecycleEvent(event),
          "background lifecycle never masquerades as a timestamp reanchor");
}

void testAndroidTimestampPreservesDeliveryAgeAndScope() {
  input::android::setInputTimestamp(92'000'123, 100'000'000, 50'000'000);
  SDL_Event event{};
  event.type = SDL_EVENT_FINGER_DOWN;
  event.common.timestamp = 50'000'000;
  require(input::android::timestampFilter(nullptr, &event) &&
              event.common.timestamp == 42'000'123,
          "Android event retains its 8 ms delivery age before SDL watchers");
  event.type = SDL_EVENT_FINGER_MOTION;
  input::android::timestampFilter(nullptr, &event);
  require(event.common.timestamp == 42'000'123,
          "every pointer in one native sample shares its original timestamp");
  event.type = SDL_EVENT_WINDOW_RESIZED;
  event.common.timestamp = 51'000'000;
  input::android::timestampFilter(nullptr, &event);
  require(event.common.timestamp == 51'000'000,
          "input dispatch cannot retimestamp unrelated lifecycle events");
  input::android::setInputTimestamp(0, 0, 0);
  event.type = SDL_EVENT_KEY_DOWN;
  input::android::timestampFilter(nullptr, &event);
  require(event.common.timestamp == 51'000'000,
          "clearing native scope restores SDL receipt timestamps");
}

void testAndroidTimestampClampsSamplesBeforeSdlStartup() {
  input::android::setInputTimestamp(1, 100'000'000, 100);
  SDL_Event event{};
  event.type = SDL_EVENT_FINGER_UP;
  input::android::timestampFilter(nullptr, &event);
  require(event.common.timestamp == 1,
          "pre-startup native samples clamp without unsigned timestamp wrap");
  input::android::setInputTimestamp(0, 0, 0);
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
  testForegroundLifecycleEventsShareOneInputPolicy();
  testAndroidTimestampPreservesDeliveryAgeAndScope();
  testAndroidTimestampClampsSamplesBeforeSdlStartup();
  return 0;
}
