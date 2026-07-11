#include "input/IOSGyroscopeMotionAdapter.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void expectNear(double actual, double expected, std::string_view message) {
  expect(std::abs(actual - expected) < 1.0e-9, message);
}

void testYawRadiansBecomeNormalizedHeadingDegrees() {
  using input::ios_gyroscope::headingDegreesFromYawRadians;

  expectNear(headingDegreesFromYawRadians(0.0), 0.0,
             "zero yaw remains zero degrees");
  expectNear(headingDegreesFromYawRadians(std::numbers::pi / 2.0), 90.0,
             "positive quarter-turn yaw becomes positive 90 degrees");
  expectNear(headingDegreesFromYawRadians(-std::numbers::pi / 2.0), -90.0,
             "negative quarter-turn yaw becomes negative 90 degrees");
  expectNear(headingDegreesFromYawRadians(2.0 * std::numbers::pi), 0.0,
             "one full yaw rotation normalizes to zero");
  expectNear(headingDegreesFromYawRadians(3.0 * std::numbers::pi / 2.0),
             -90.0, "heading normalization chooses the shortest signed angle");
}

void testWorldVerticalRateProjectionKeepsClockwisePositive() {
  using input::ios_gyroscope::Vector3;
  using input::ios_gyroscope::clockwiseWorldVerticalRateDegreesPerSecond;

  const Vector3 flatGravity{.x = 0.0, .y = 0.0, .z = -1.0};
  expectNear(clockwiseWorldVerticalRateDegreesPerSecond(
                 {.x = 0.0, .y = 0.0, .z = -std::numbers::pi / 2.0},
                 flatGravity),
             90.0,
             "negative device-Z rotation is clockwise for a screen-up device");
  expectNear(clockwiseWorldVerticalRateDegreesPerSecond(
                 {.x = 0.0, .y = 0.0, .z = std::numbers::pi / 2.0},
                 flatGravity),
             -90.0,
             "positive device-Z rotation is counter-clockwise when screen-up");

  expectNear(clockwiseWorldVerticalRateDegreesPerSecond(
                 {.x = std::numbers::pi, .y = std::numbers::pi / 2.0, .z = 0.0},
                 {.x = 0.5, .y = 0.5, .z = std::sqrt(0.5)}),
             135.0,
             "rotation rate is projected onto gravity for a tilted device");
}

void testAccuracyGenerationChangesOnlyAcrossMeaningfulTiers() {
  using input::ios_gyroscope::AccuracyGenerationTracker;
  using input::ios_gyroscope::MagneticAccuracy;

  AccuracyGenerationTracker tracker;
  auto decision = tracker.observe(MagneticAccuracy::Low);
  expect(!decision.usable && decision.generation == 0 &&
             !decision.generationChanged,
         "the first unusable accuracy establishes generation zero");

  decision = tracker.observe(MagneticAccuracy::Uncalibrated);
  expect(!decision.usable && decision.generation == 0 &&
             !decision.generationChanged,
         "switching between unusable tiers does not force another baseline");

  decision = tracker.observe(MagneticAccuracy::Medium);
  expect(decision.usable && decision.generation == 1 &&
             decision.generationChanged,
         "becoming usable advances the accuracy generation");

  decision = tracker.observe(MagneticAccuracy::Medium);
  expect(decision.usable && decision.generation == 1 &&
             !decision.generationChanged,
         "a repeated usable tier keeps its generation");

  decision = tracker.observe(MagneticAccuracy::High);
  expect(decision.usable && decision.generation == 2 &&
             decision.generationChanged,
         "Medium to High advances the accuracy generation");

  decision = tracker.observe(MagneticAccuracy::Low);
  expect(!decision.usable && decision.generation == 3 &&
             decision.generationChanged,
         "losing usable accuracy advances the accuracy generation");

  tracker.reset();
  decision = tracker.observe(MagneticAccuracy::High);
  expect(decision.usable && decision.generation == 0 &&
             !decision.generationChanged,
         "a sensor restart begins with a fresh generation-zero baseline");
}

void testReferenceFramePolicyRejectsSimulatorAndUncorrectedMotion() {
  using input::ios_gyroscope::hasRequiredMotionHardware;
  using input::ios_gyroscope::ReferenceFrameChoice;
  using input::ios_gyroscope::chooseReferenceFrame;

  expect(!hasRequiredMotionHardware(true, true, true, true),
         "iOS Simulator never advertises physical motion hardware");
  expect(hasRequiredMotionHardware(false, true, true, true),
         "device motion, gyroscope, and magnetometer are required hardware");
  expect(!hasRequiredMotionHardware(false, true, true, false),
         "missing magnetometer cannot provide compass correction");

  expect(chooseReferenceFrame(true, true, true, true) ==
             ReferenceFrameChoice::Unsupported,
         "iOS Simulator never advertises a gyroscope turntable");
  expect(chooseReferenceFrame(false, false, true, true) ==
             ReferenceFrameChoice::Unsupported,
         "missing device motion is unsupported");
  expect(chooseReferenceFrame(false, true, true, true) ==
             ReferenceFrameChoice::ArbitraryCorrectedZVertical,
         "arbitrary corrected Z-vertical is preferred");
  expect(chooseReferenceFrame(false, true, false, true) ==
             ReferenceFrameChoice::MagneticNorthZVertical,
         "magnetic-north Z-vertical is the corrected fallback");
  expect(chooseReferenceFrame(false, true, false, false) ==
             ReferenceFrameChoice::Unsupported,
         "corrected-frame readiness is separate from hardware support");
}

void testReferenceFrameAttemptReprobesAfterTemporaryUnavailability() {
  using input::ios_gyroscope::ReferenceFrameAvailability;
  using input::ios_gyroscope::ReferenceFrameChoice;
  using input::ios_gyroscope::probeReferenceFrameForAttempt;

  int probeCount = 0;
  const auto probe = [&]() {
    ++probeCount;
    return probeCount == 1
               ? ReferenceFrameAvailability{
                     .deviceMotionAvailable = true,
                     .arbitraryCorrectedZVerticalAvailable = false,
                     .magneticNorthZVerticalAvailable = false}
               : ReferenceFrameAvailability{
                     .deviceMotionAvailable = true,
                     .arbitraryCorrectedZVerticalAvailable = true,
                     .magneticNorthZVerticalAvailable = false};
  };

  expect(probeReferenceFrameForAttempt(false, probe) ==
             ReferenceFrameChoice::Unsupported,
         "the first attempt waits when no corrected frame is available");
  expect(probeReferenceFrameForAttempt(false, probe) ==
             ReferenceFrameChoice::ArbitraryCorrectedZVertical,
         "a retry observes a corrected frame that became available later");
  expect(probeCount == 2, "each native start attempt performs a fresh probe");
}

} // namespace

int main() {
  testYawRadiansBecomeNormalizedHeadingDegrees();
  testWorldVerticalRateProjectionKeepsClockwisePositive();
  testAccuracyGenerationChangesOnlyAcrossMeaningfulTiers();
  testReferenceFramePolicyRejectsSimulatorAndUncorrectedMotion();
  testReferenceFrameAttemptReprobesAfterTemporaryUnavailability();

  if (failures != 0) {
    std::cerr << failures << " iOS gyroscope adapter assertion(s) failed\n";
    return 1;
  }
  std::cout << "iOS gyroscope motion adapter tests passed\n";
  return 0;
}
