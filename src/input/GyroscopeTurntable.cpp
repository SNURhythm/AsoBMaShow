#include "GyroscopeTurntable.h"

#include <algorithm>
#include <cmath>

namespace input {
namespace {

constexpr double kGyroscopeNoiseFloorDegreesPerSecond = 0.5;
constexpr double kMaximumSensorGapSeconds = 0.250;
constexpr double kMaximumRateDegreesPerSecond = 1080.0;
constexpr double kMinimumFusionToleranceDegrees = 0.75;
constexpr double kFusionToleranceRatio = 0.5;

double shortestHeadingDelta(double current, double previous) {
  double delta = std::fmod(current - previous, 360.0);
  if (delta > 180.0) {
    delta -= 360.0;
  } else if (delta < -180.0) {
    delta += 360.0;
  }
  return delta;
}

bool sameNonzeroSign(double left, double right) {
  return left != 0.0 && right != 0.0 &&
         std::signbit(left) == std::signbit(right);
}

} // namespace

void GyroscopeTurntableConfig::sanitize(std::vector<std::string> &diagnostics) {
  const int sanitizedStep =
      std::clamp(stepAngleDegrees, kMinStepAngleDegrees, kMaxStepAngleDegrees);
  if (sanitizedStep != stepAngleDegrees) {
    stepAngleDegrees = sanitizedStep;
    diagnostics.emplace_back("Clamped gyroscope turntable step angle.");
  }

  const int sanitizedDelay =
      std::clamp(releaseDelayMs, kMinReleaseDelayMs, kMaxReleaseDelayMs);
  if (sanitizedDelay != releaseDelayMs) {
    releaseDelayMs = sanitizedDelay;
    diagnostics.emplace_back("Clamped gyroscope turntable release delay.");
  }
}

GyroscopeTurntable::GyroscopeTurntable(GyroscopeTurntableConfig config)
    : config_(config) {
  std::vector<std::string> ignoredDiagnostics;
  config_.sanitize(ignoredDiagnostics);
}

std::optional<float>
GyroscopeTurntable::observe(const GyroscopeMotionSample &sample,
                            std::uint64_t monotonicNowMicros) {
  if (!std::isfinite(sample.sensorTimestampSeconds) ||
      sample.sensorTimestampSeconds < 0.0) {
    return clearState();
  }

  if (hasBaseline_ &&
      sample.sensorTimestampSeconds == previousSensorTimestampSeconds_) {
    return std::nullopt;
  }

  if (!std::isfinite(sample.headingDegrees) ||
      !std::isfinite(sample.clockwiseRateDegreesPerSecond) ||
      !sample.usableAccuracy || sample.discontinuity ||
      std::abs(sample.clockwiseRateDegreesPerSecond) >
          kMaximumRateDegreesPerSecond) {
    return clearState();
  }

  if (!hasBaseline_) {
    establishBaseline(sample);
    return std::nullopt;
  }

  if (sample.accuracyGeneration != accuracyGeneration_) {
    const auto transition = clearState();
    establishBaseline(sample);
    return transition;
  }

  const double sensorDeltaSeconds =
      sample.sensorTimestampSeconds - previousSensorTimestampSeconds_;
  if (sensorDeltaSeconds < 0.0 ||
      sensorDeltaSeconds > kMaximumSensorGapSeconds ||
      std::abs(previousRateDegreesPerSecond_) > kMaximumRateDegreesPerSecond) {
    return clearState();
  }

  const double headingDelta =
      shortestHeadingDelta(sample.headingDegrees, previousHeadingDegrees_);
  const double gyroDelta =
      (previousRateDegreesPerSecond_ + sample.clockwiseRateDegreesPerSecond) *
      0.5 * sensorDeltaSeconds;
  const bool stationary = std::abs(previousRateDegreesPerSecond_) <=
                              kGyroscopeNoiseFloorDegreesPerSecond &&
                          std::abs(sample.clockwiseRateDegreesPerSecond) <=
                              kGyroscopeNoiseFloorDegreesPerSecond;

  establishBaseline(sample);
  if (stationary) {
    return std::nullopt;
  }

  const double tolerance =
      std::max(kMinimumFusionToleranceDegrees,
               std::abs(gyroDelta) * kFusionToleranceRatio);
  const double acceptedDelta =
      sameNonzeroSign(headingDelta, gyroDelta) &&
              std::abs(headingDelta - gyroDelta) <= tolerance
          ? headingDelta
          : gyroDelta;
  if (acceptedDelta == 0.0) {
    return std::nullopt;
  }

  if (accumulatedDegrees_ != 0.0 &&
      !sameNonzeroSign(accumulatedDegrees_, acceptedDelta)) {
    accumulatedDegrees_ = 0.0;
  }
  accumulatedDegrees_ += acceptedDelta;

  const double step = static_cast<double>(config_.stepAngleDegrees);
  const double completedSteps = std::trunc(accumulatedDegrees_ / step);
  if (completedSteps == 0.0) {
    return std::nullopt;
  }

  accumulatedDegrees_ -= completedSteps * step;
  lastCompletedStepMicros_ = monotonicNowMicros;
  const float nextValue = completedSteps > 0.0 ? 1.0F : -1.0F;
  if (nextValue == value_) {
    return std::nullopt;
  }
  value_ = nextValue;
  return value_;
}

std::optional<float>
GyroscopeTurntable::advance(std::uint64_t monotonicNowMicros) {
  if (value_ == 0.0F || !lastCompletedStepMicros_.has_value() ||
      monotonicNowMicros < *lastCompletedStepMicros_) {
    return std::nullopt;
  }
  const std::uint64_t releaseDelayMicros =
      static_cast<std::uint64_t>(config_.releaseDelayMs) * 1000ULL;
  if (monotonicNowMicros - *lastCompletedStepMicros_ < releaseDelayMicros) {
    return std::nullopt;
  }

  value_ = 0.0F;
  accumulatedDegrees_ = 0.0;
  lastCompletedStepMicros_.reset();
  return 0.0F;
}

std::optional<float>
GyroscopeTurntable::configure(GyroscopeTurntableConfig config) {
  std::vector<std::string> ignoredDiagnostics;
  config.sanitize(ignoredDiagnostics);
  config_ = config;
  return clearState();
}

std::optional<float> GyroscopeTurntable::reset() { return clearState(); }

std::optional<float> GyroscopeTurntable::clearState() {
  const bool wasActive = value_ != 0.0F;
  hasBaseline_ = false;
  previousHeadingDegrees_ = 0.0;
  previousRateDegreesPerSecond_ = 0.0;
  previousSensorTimestampSeconds_ = 0.0;
  accuracyGeneration_ = 0;
  accumulatedDegrees_ = 0.0;
  value_ = 0.0F;
  lastCompletedStepMicros_.reset();
  return wasActive ? std::optional<float>{0.0F} : std::nullopt;
}

void GyroscopeTurntable::establishBaseline(
    const GyroscopeMotionSample &sample) {
  hasBaseline_ = true;
  previousHeadingDegrees_ = sample.headingDegrees;
  previousRateDegreesPerSecond_ = sample.clockwiseRateDegreesPerSecond;
  previousSensorTimestampSeconds_ = sample.sensorTimestampSeconds;
  accuracyGeneration_ = sample.accuracyGeneration;
}

} // namespace input
