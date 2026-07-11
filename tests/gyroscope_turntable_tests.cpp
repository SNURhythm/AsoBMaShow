#include "input/GyroscopeTurntable.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

input::GyroscopeMotionSample sample(double heading, double rate,
                                    double timestamp,
                                    std::uint64_t accuracyGeneration = 1,
                                    bool usableAccuracy = true,
                                    bool discontinuity = false) {
  return {.headingDegrees = heading,
          .clockwiseRateDegreesPerSecond = rate,
          .sensorTimestampSeconds = timestamp,
          .accuracyGeneration = accuracyGeneration,
          .usableAccuracy = usableAccuracy,
          .discontinuity = discontinuity};
}

void requireNoTransition(const std::optional<float> &transition,
                         std::string_view message) {
  require(!transition.has_value(), message);
}

void requireTransition(const std::optional<float> &transition, float expected,
                       std::string_view message) {
  require(transition.has_value() && *transition == expected, message);
}

void testBaselinesAndBothDirections() {
  input::GyroscopeTurntable turntable;
  requireNoTransition(turntable.observe(sample(10.0, 30.0, 1.0), 0),
                      "first usable sample only establishes the baseline");
  requireTransition(turntable.observe(sample(13.0, 30.0, 1.1), 100000), 1.0F,
                    "three clockwise degrees activates positive output");
  requireTransition(turntable.reset(), 0.0F,
                    "reset releases an active direction");
  requireNoTransition(turntable.observe(sample(13.0, -30.0, 2.0), 200000),
                      "first post-reset sample re-baselines");
  requireTransition(
      turntable.observe(sample(10.0, -30.0, 2.1), 300000), -1.0F,
      "three counter-clockwise degrees activates negative output");
}

void testSubstepsRemaindersWrapAndReversal() {
  input::GyroscopeTurntable turntable;
  requireNoTransition(turntable.observe(sample(0.0, 10.0, 0.0), 0),
                      "baseline is silent");
  requireNoTransition(turntable.observe(sample(1.0, 10.0, 0.1), 100000),
                      "one degree remains below the step");
  requireNoTransition(turntable.observe(sample(2.0, 10.0, 0.2), 200000),
                      "two degrees remain below the step");
  requireTransition(turntable.observe(sample(4.0, 20.0, 0.3), 300000), 1.0F,
                    "whole step activates and retains one degree remainder");
  requireNoTransition(turntable.observe(sample(6.0, 20.0, 0.4), 400000),
                      "retained remainder completes a repeated step silently");
  requireNoTransition(turntable.observe(sample(6.0, -20.0, 0.5), 500000),
                      "rate-sign bridge contributes no movement");
  requireTransition(turntable.observe(sample(3.0, -30.0, 0.6), 600000), -1.0F,
                    "a complete opposite step reverses immediately");

  input::GyroscopeTurntable clockwiseWrap;
  requireNoTransition(clockwiseWrap.observe(sample(359.0, 20.0, 1.0), 0),
                      "wrap fixture baselines");
  requireNoTransition(clockwiseWrap.observe(sample(1.0, 20.0, 1.1), 100000),
                      "359 to 1 accumulates two clockwise degrees");
  requireTransition(clockwiseWrap.observe(sample(2.0, 10.0, 1.2), 200000), 1.0F,
                    "clockwise wrap reaches the configured step");

  input::GyroscopeTurntable counterWrap;
  requireNoTransition(counterWrap.observe(sample(1.0, -20.0, 1.0), 0),
                      "negative wrap fixture baselines");
  requireNoTransition(counterWrap.observe(sample(359.0, -20.0, 1.1), 100000),
                      "1 to 359 accumulates two counter-clockwise degrees");
  requireTransition(counterWrap.observe(sample(358.0, -10.0, 1.2), 200000),
                    -1.0F, "counter-clockwise wrap reaches the step");
}

void testDirectionChangeClearsPartialMovement() {
  input::GyroscopeTurntable turntable;
  requireNoTransition(turntable.observe(sample(0.0, 20.0, 0.0), 0),
                      "baseline is silent");
  requireNoTransition(turntable.observe(sample(2.0, 20.0, 0.1), 100000),
                      "clockwise remainder accumulates");
  requireNoTransition(turntable.observe(sample(1.0, -10.0, 0.2), 200000),
                      "reversal clears the clockwise remainder");
  requireNoTransition(turntable.observe(sample(359.0, -20.0, 0.3), 300000),
                      "two further negative degrees are still sub-step");
  requireTransition(turntable.observe(sample(358.0, -10.0, 0.4), 400000), -1.0F,
                    "only a fresh full opposite step activates");
}

void testDeadlineRefreshEqualTimestampAndRemainderClear() {
  input::GyroscopeTurntable turntable;
  requireNoTransition(turntable.observe(sample(0.0, 30.0, 0.0), 0),
                      "baseline is silent");
  requireTransition(turntable.observe(sample(3.0, 30.0, 0.1), 100000), 1.0F,
                    "first step activates");
  requireNoTransition(turntable.observe(sample(6.0, 30.0, 0.2), 250000),
                      "same direction refreshes without duplicate output");
  requireNoTransition(turntable.observe(sample(180.0, -999.0, 0.2), 260000),
                      "equal timestamps are ignored completely");
  requireNoTransition(turntable.advance(449999),
                      "axis stays active before refreshed deadline");
  requireTransition(turntable.advance(450000), 0.0F,
                    "advance releases exactly at refreshed deadline");
  requireNoTransition(turntable.observe(sample(7.0, 10.0, 0.3), 500000),
                      "one post-release degree does not reuse old remainder");
}

void testCompassDriftNoiseFloorAndDisagreementFallback() {
  input::GyroscopeTurntable stationary;
  requireNoTransition(stationary.observe(sample(0.0, 0.0, 0.0), 0),
                      "stationary fixture baselines");
  for (int degree = 1; degree <= 20; ++degree) {
    requireNoTransition(
        stationary.observe(sample(static_cast<double>(degree), 0.49,
                                  static_cast<double>(degree) * 0.02),
                           static_cast<std::uint64_t>(degree) * 20000),
        "gradual compass correction below the gyro floor is silent");
  }
  require(stationary.value() == 0.0F,
          "stationary compass drift never changes the axis");

  input::GyroscopeTurntable exactNoiseFloor;
  requireNoTransition(exactNoiseFloor.observe(sample(0.0, 0.5, 0.0), 0),
                      "noise-floor fixture baselines");
  requireNoTransition(exactNoiseFloor.observe(sample(10.0, 0.5, 0.1), 100000),
                      "the exact 0.5 dps noise floor remains stationary");

  input::GyroscopeTurntable oneMovingEndpoint;
  requireNoTransition(oneMovingEndpoint.observe(sample(0.0, 0.0, 0.0), 0),
                      "endpoint fixture baselines");
  requireTransition(
      oneMovingEndpoint.observe(sample(3.0, 24.0, 0.25), 250000), 1.0F,
      "a moving endpoint across the exact 250 ms limit is accepted");

  input::GyroscopeTurntable disagreement;
  requireNoTransition(disagreement.observe(sample(0.0, 10.0, 0.0), 0),
                      "disagreement fixture baselines");
  requireNoTransition(disagreement.observe(sample(20.0, 10.0, 0.1), 100000),
                      "large compass correction falls back to one gyro degree");
  requireNoTransition(disagreement.observe(sample(40.0, 10.0, 0.2), 200000),
                      "second disagreement contributes only one gyro degree");
  requireTransition(disagreement.observe(sample(60.0, 10.0, 0.3), 300000), 1.0F,
                    "three intervals of gyro-confirmed motion make one step");

  input::GyroscopeTurntable signMismatch;
  requireNoTransition(signMismatch.observe(sample(0.0, 30.0, 0.0), 0),
                      "sign fixture baselines");
  requireTransition(signMismatch.observe(sample(-20.0, 30.0, 0.1), 100000),
                    1.0F,
                    "heading with the wrong sign cannot reverse gyro motion");
}

void testDiscontinuitiesAccuracyAndGenerationReseed() {
  auto active = [] {
    input::GyroscopeTurntable result;
    requireNoTransition(result.observe(sample(0.0, 30.0, 0.0), 0),
                        "active fixture baselines");
    requireTransition(result.observe(sample(3.0, 30.0, 0.1), 100000), 1.0F,
                      "active fixture activates");
    return result;
  };

  {
    auto turntable = active();
    requireTransition(
        turntable.observe(sample(6.0, 30.0, 0.2, 1, false), 200000), 0.0F,
        "loss of usable accuracy releases immediately");
    requireNoTransition(turntable.observe(sample(9.0, 30.0, 0.3), 300000),
                        "accuracy recovery only re-baselines");
  }
  {
    auto turntable = active();
    requireTransition(turntable.observe(sample(6.0, 30.0, 0.2, 2), 200000),
                      0.0F, "accuracy-generation change releases and reseeds");
    requireTransition(turntable.observe(sample(9.0, 30.0, 0.3, 2), 300000),
                      1.0F, "motion resumes after the generation baseline");
  }
  {
    auto turntable = active();
    requireTransition(
        turntable.observe(sample(6.0, 30.0, 0.2, 1, true, true), 200000), 0.0F,
        "explicit discontinuity releases");
    requireNoTransition(turntable.observe(sample(9.0, 30.0, 0.3), 300000),
                        "sample after discontinuity becomes a baseline");
  }

  const std::vector<input::GyroscopeMotionSample> invalidSamples = {
      sample(std::numeric_limits<double>::quiet_NaN(), 30.0, 0.2),
      sample(6.0, std::numeric_limits<double>::infinity(), 0.2),
      sample(6.0, 30.0, std::numeric_limits<double>::quiet_NaN()),
      sample(6.0, 30.0, 0.05),
      sample(20.0, 30.0, 0.5),
      sample(6.0, 1080.01, 0.2),
  };
  for (const auto &invalid : invalidSamples) {
    auto turntable = active();
    requireTransition(turntable.observe(invalid, 200000), 0.0F,
                      "invalid or discontinuous motion releases output");
  }

  {
    auto turntable = active();
    requireNoTransition(turntable.observe(sample(4.08, 1080.0, 0.101), 101000),
                        "the exact maximum sensor rate remains valid");
    require(turntable.value() == 1.0F,
            "the exact maximum rate does not release active output");
  }
}

void testConfigurationSanitizationAndReplacement() {
  input::GyroscopeTurntableConfig low{.stepAngleDegrees = -4,
                                      .releaseDelayMs = 20};
  std::vector<std::string> diagnostics;
  low.sanitize(diagnostics);
  require(low.stepAngleDegrees == 1 && low.releaseDelayMs == 50,
          "configuration clamps both low boundaries independently");
  require(diagnostics.size() == 2,
          "each independently clamped field emits a diagnostic");

  input::GyroscopeTurntableConfig high{.stepAngleDegrees = 99,
                                       .releaseDelayMs = 5000};
  diagnostics.clear();
  high.sanitize(diagnostics);
  require(high.stepAngleDegrees == 45 && high.releaseDelayMs == 1000,
          "configuration clamps both high boundaries independently");
  diagnostics.clear();
  high.sanitize(diagnostics);
  require(diagnostics.empty(), "sanitizing valid boundaries is idempotent");

  input::GyroscopeTurntable turntable;
  requireNoTransition(turntable.observe(sample(0.0, 30.0, 0.0), 0),
                      "config fixture baselines");
  requireTransition(turntable.observe(sample(3.0, 30.0, 0.1), 100000), 1.0F,
                    "config fixture activates");
  requireTransition(
      turntable.configure({.stepAngleDegrees = 6, .releaseDelayMs = 400}), 0.0F,
      "configuration replacement releases active output");
  require(turntable.config().stepAngleDegrees == 6 &&
              turntable.config().releaseDelayMs == 400,
          "replacement applies sanitized values");
  requireNoTransition(turntable.observe(sample(6.0, 60.0, 0.2), 200000),
                      "first sample after configuration re-baselines");
  requireNoTransition(turntable.observe(sample(9.0, 30.0, 0.3), 300000),
                      "old partial movement was discarded");
}

} // namespace

int main() {
  try {
    testBaselinesAndBothDirections();
    testSubstepsRemaindersWrapAndReversal();
    testDirectionChangeClearsPartialMovement();
    testDeadlineRefreshEqualTimestampAndRemainderClear();
    testCompassDriftNoiseFloorAndDisagreementFallback();
    testDiscontinuitiesAccuracyAndGenerationReseed();
    testConfigurationSanitizationAndReplacement();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "gyroscope_turntable_tests: " << error.what() << '\n';
    return 1;
  }
}
