#include "replay/ReplayLimits.h"

#include <array>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testDefaultResourceLimitsArePinned() {
  constexpr auto limits = replay::kReplayLimits;
  expect(limits.valid(), "default replay limits are internally consistent");
  expect(limits.minimumSongTimeMicros == -30'000'000,
         "pre-roll is exactly thirty seconds");
  expect(limits.maxCompressedBytes == 64U * 1024U * 1024U,
         "compressed file bound is 64 MiB");
  expect(limits.maxJsonBytes == 256U * 1024U * 1024U,
         "expanded JSON bound is 256 MiB");
  expect(limits.maxKeyInputBytes == 9U * 1'000'000U,
         "key-input byte bound holds one million records");
  expect(limits.maxInputTransitions == 1'000'000U &&
             limits.maxTouchSamples == 1'000'000U &&
             limits.maxLaneCoverEvents == 100'000U &&
             limits.maxRandomValues == 100'000U,
         "raw replay collection bounds are pinned");
  expect(limits.maxCourseStages == 256U &&
             limits.maxCourseRestMicros == 3'600'000'000LL,
         "course stage/rest bounds are pinned");
  expect(limits.maxJsonDepth == 64U &&
             limits.maxStringBytes == 16U * 1024U &&
             limits.maxFilenameBytes == 255U,
         "JSON, string, and filename bounds are pinned");
}

void testSignedSongTimeUsesInclusiveAttemptBounds() {
  constexpr replay::ReplayTimeBounds bounds{
      .completionSongTimeMicros = 5'000'000,
  };
  expect(bounds.valid(), "nonnegative completion time is valid");
  expect(bounds.contains(-30'000'000),
         "exact thirty-second pre-roll is accepted");
  expect(!bounds.contains(-30'000'001),
         "timestamp before pre-roll is rejected");
  expect(bounds.contains(5'000'000),
         "exact completion timestamp is accepted");
  expect(!bounds.contains(5'000'001),
         "timestamp after completion is rejected");

  constexpr replay::ReplayTimeBounds invalid{
      .completionSongTimeMicros = -1,
  };
  expect(!invalid.valid() && !invalid.contains(-1),
         "negative completion boundary rejects every timestamp");
}

void testOrderingIsMonotonicAndNeverSortedIntoValidity() {
  constexpr replay::ReplayTimeBounds bounds{
      .completionSongTimeMicros = 10'000'000,
  };
  expect(replay::isMonotonicReplayTime(-1'000'000, -1'000'000, bounds),
         "equal adjacent timestamps are valid");
  expect(replay::isMonotonicReplayTime(-1'000'000, 0, bounds),
         "increasing adjacent timestamps are valid");
  expect(!replay::isMonotonicReplayTime(1, 0, bounds),
         "decreasing adjacent timestamps are rejected");
  expect(!replay::isMonotonicReplayTime(0, 10'000'001, bounds),
         "ordered timestamp still respects completion");
}

void testCourseRestUsesOnePredicateAndClamp() {
  expect(replay::validCourseRestMicros(0), "zero rest is valid");
  expect(replay::validCourseRestMicros(3'600'000'000LL),
         "exact one-hour rest is valid");
  expect(!replay::validCourseRestMicros(-1), "negative rest is invalid");
  expect(!replay::validCourseRestMicros(3'600'000'001LL),
         "rest above one hour is invalid");
  expect(replay::clampCourseRestMicros(-1) == 0,
         "producer clamp floors negative rest");
  expect(replay::clampCourseRestMicros(3'600'000'001LL) ==
             3'600'000'000LL,
         "producer clamp caps oversized rest");
}

void testCountsUseInclusiveUpperBounds() {
  constexpr auto limits = replay::kReplayLimits;
  constexpr std::array maxima{
      limits.maxInputTransitions,
      limits.maxTouchSamples,
      limits.maxLaneCoverEvents,
      limits.maxRandomValues,
      limits.maxCourseStages,
  };
  for (std::size_t maximum : maxima) {
    expect(replay::withinReplayCountLimit(maximum, maximum),
           "exact collection maximum is accepted");
    expect(!replay::withinReplayCountLimit(maximum + 1, maximum),
           "collection count above maximum is rejected");
  }
}

void testMalformedCustomLimitSetsFailClosed() {
  replay::ReplayLimits zero = replay::kReplayLimits;
  zero.maxInputTransitions = 0;
  expect(!zero.valid(), "zero collection limit is invalid");

  replay::ReplayLimits inverted = replay::kReplayLimits;
  inverted.maxCompressedBytes = inverted.maxJsonBytes + 1;
  expect(!inverted.valid(), "compressed bound cannot exceed JSON bound");

  replay::ReplayLimits positivePreRoll = replay::kReplayLimits;
  positivePreRoll.minimumSongTimeMicros = 1;
  expect(!positivePreRoll.valid(), "minimum song time cannot be positive");
}

} // namespace

int main() {
  testDefaultResourceLimitsArePinned();
  testSignedSongTimeUsesInclusiveAttemptBounds();
  testOrderingIsMonotonicAndNeverSortedIntoValidity();
  testCourseRestUsesOnePredicateAndClamp();
  testCountsUseInclusiveUpperBounds();
  testMalformedCustomLimitSetsFailClosed();
  if (failures != 0) {
    std::cerr << failures << " replay limit test(s) failed\n";
    return 1;
  }
  std::cout << "replay limit tests passed\n";
  return 0;
}
