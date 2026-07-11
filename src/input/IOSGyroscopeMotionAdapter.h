#pragma once

#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>

namespace input::ios_gyroscope {

struct Vector3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

inline double headingDegreesFromYawRadians(double yawRadians) {
  const double degrees =
      yawRadians * 180.0 / std::numbers::pi_v<double>;
  double normalized = std::remainder(degrees, 360.0);
  if (normalized == -0.0) {
    normalized = 0.0;
  }
  return normalized;
}

inline double clockwiseWorldVerticalRateDegreesPerSecond(
    const Vector3 &rotationRateRadiansPerSecond, const Vector3 &gravity) {
  const double projectedRadiansPerSecond =
      rotationRateRadiansPerSecond.x * gravity.x +
      rotationRateRadiansPerSecond.y * gravity.y +
      rotationRateRadiansPerSecond.z * gravity.z;
  return projectedRadiansPerSecond * 180.0 / std::numbers::pi_v<double>;
}

enum class MagneticAccuracy : int {
  Uncalibrated = -1,
  Low = 0,
  Medium = 1,
  High = 2,
};

inline bool isUsable(MagneticAccuracy accuracy) {
  return accuracy == MagneticAccuracy::Medium ||
         accuracy == MagneticAccuracy::High;
}

struct AccuracyDecision {
  bool usable = false;
  std::uint64_t generation = 0;
  bool generationChanged = false;
};

class AccuracyGenerationTracker {
public:
  AccuracyDecision observe(MagneticAccuracy accuracy) {
    const bool usable = isUsable(accuracy);
    bool changed = false;
    if (initialized_ &&
        (usable != usable_ || (usable && accuracy != accuracy_))) {
      ++generation_;
      changed = true;
    }
    initialized_ = true;
    usable_ = usable;
    accuracy_ = accuracy;
    return {.usable = usable,
            .generation = generation_,
            .generationChanged = changed};
  }

  void reset() {
    initialized_ = false;
    usable_ = false;
    accuracy_ = MagneticAccuracy::Uncalibrated;
    generation_ = 0;
  }

private:
  bool initialized_ = false;
  bool usable_ = false;
  MagneticAccuracy accuracy_ = MagneticAccuracy::Uncalibrated;
  std::uint64_t generation_ = 0;
};

enum class ReferenceFrameChoice {
  Unsupported,
  ArbitraryCorrectedZVertical,
  MagneticNorthZVertical,
};

struct ReferenceFrameAvailability {
  bool deviceMotionAvailable = false;
  bool arbitraryCorrectedZVerticalAvailable = false;
  bool magneticNorthZVerticalAvailable = false;
};

constexpr bool hasRequiredMotionHardware(bool simulator,
                                         bool deviceMotionAvailable,
                                         bool gyroscopeAvailable,
                                         bool magnetometerAvailable) {
  return !simulator && deviceMotionAvailable && gyroscopeAvailable &&
         magnetometerAvailable;
}

constexpr ReferenceFrameChoice chooseReferenceFrame(
    bool simulator, bool deviceMotionAvailable,
    bool arbitraryCorrectedZVerticalAvailable,
    bool magneticNorthZVerticalAvailable) {
  if (simulator || !deviceMotionAvailable) {
    return ReferenceFrameChoice::Unsupported;
  }
  if (arbitraryCorrectedZVerticalAvailable) {
    return ReferenceFrameChoice::ArbitraryCorrectedZVertical;
  }
  if (magneticNorthZVerticalAvailable) {
    return ReferenceFrameChoice::MagneticNorthZVertical;
  }
  return ReferenceFrameChoice::Unsupported;
}

template <typename Probe>
ReferenceFrameChoice probeReferenceFrameForAttempt(bool simulator,
                                                   Probe &&probe) {
  const ReferenceFrameAvailability availability =
      std::forward<Probe>(probe)();
  return chooseReferenceFrame(
      simulator, availability.deviceMotionAvailable,
      availability.arbitraryCorrectedZVerticalAvailable,
      availability.magneticNorthZVerticalAvailable);
}

} // namespace input::ios_gyroscope
