#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace input {

inline constexpr std::string_view kGyroscopeTurntableStableId =
    "builtin:gyroscope-turntable";
inline constexpr std::string_view kGyroscopeTurntableDisplayName =
    "Gyroscope Turntable";
inline constexpr int kGyroscopeTurntableAxis = 0;

struct GyroscopeTurntableConfig {
  static constexpr int kDefaultStepAngleDegrees = 3;
  static constexpr int kMinStepAngleDegrees = 1;
  static constexpr int kMaxStepAngleDegrees = 45;
  static constexpr int kDefaultReleaseDelayMs = 200;
  static constexpr int kMinReleaseDelayMs = 50;
  static constexpr int kMaxReleaseDelayMs = 1000;

  int stepAngleDegrees = kDefaultStepAngleDegrees;
  int releaseDelayMs = kDefaultReleaseDelayMs;

  auto operator<=>(const GyroscopeTurntableConfig &) const = default;

  void sanitize(std::vector<std::string> &diagnostics);
};

struct GyroscopeMotionSample {
  double headingDegrees = 0.0;
  double clockwiseRateDegreesPerSecond = 0.0;
  double sensorTimestampSeconds = 0.0;
  std::uint64_t accuracyGeneration = 0;
  bool usableAccuracy = false;
  bool discontinuity = false;
};

class GyroscopeTurntable {
public:
  explicit GyroscopeTurntable(GyroscopeTurntableConfig config = {});

  std::optional<float> observe(const GyroscopeMotionSample &sample,
                               std::uint64_t monotonicNowMicros);
  std::optional<float> advance(std::uint64_t monotonicNowMicros);
  std::optional<float> configure(GyroscopeTurntableConfig config);
  std::optional<float> reset();

  [[nodiscard]] float value() const { return value_; }
  [[nodiscard]] const GyroscopeTurntableConfig &config() const {
    return config_;
  }

private:
  std::optional<float> clearState();
  void establishBaseline(const GyroscopeMotionSample &sample);

  GyroscopeTurntableConfig config_;
  bool hasBaseline_ = false;
  double previousHeadingDegrees_ = 0.0;
  double previousRateDegreesPerSecond_ = 0.0;
  double previousSensorTimestampSeconds_ = 0.0;
  std::uint64_t accuracyGeneration_ = 0;
  double accumulatedDegrees_ = 0.0;
  float value_ = 0.0F;
  std::optional<std::uint64_t> lastCompletedStepMicros_;
};

} // namespace input
