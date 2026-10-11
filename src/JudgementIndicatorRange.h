#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace judgement_indicator {
inline constexpr int kDefaultRangeMilliseconds = 180;
inline constexpr int kMinRangeMilliseconds = 1;
inline constexpr int kMaxRangeMilliseconds = 1000;
// The presentation snapshot retains the same bounded recent-judge window as
// the renderer, so preparing an immutable frame cannot collapse it to only
// the latest judgement.
inline constexpr std::size_t kRecentTimingSampleCapacity = 96;

[[nodiscard]] constexpr int
sanitizeStoredRangeMilliseconds(int value) noexcept {
  if (value <= 0) {
    return kDefaultRangeMilliseconds;
  }
  return std::min(value, kMaxRangeMilliseconds);
}

[[nodiscard]] constexpr int
clampEditableRangeMilliseconds(int value) noexcept {
  return std::clamp(value, kMinRangeMilliseconds, kMaxRangeMilliseconds);
}

[[nodiscard]] constexpr std::int64_t rangeMicros(int milliseconds) noexcept {
  return static_cast<std::int64_t>(
             sanitizeStoredRangeMilliseconds(milliseconds)) *
         1000LL;
}

[[nodiscard]] inline std::string formatRangeLabel(int milliseconds) {
  return "+/-" +
         std::to_string(clampEditableRangeMilliseconds(milliseconds)) +
         " ms";
}

[[nodiscard]] constexpr float
normalizedOffset(std::int64_t diffMicros,
                 std::int64_t displayRangeMicros) noexcept {
  const std::int64_t safeRange = std::max<std::int64_t>(1, displayRangeMicros);
  const float raw =
      static_cast<float>(diffMicros) / static_cast<float>(safeRange);
  return std::clamp(raw, -1.0f, 1.0f);
}

struct Segment {
  std::int64_t startMicros = 0;
  std::int64_t endMicros = 0;
};

[[nodiscard]] constexpr std::optional<Segment>
clipSegment(std::int64_t startMicros, std::int64_t endMicros,
            std::int64_t displayRangeMicros) noexcept {
  const std::int64_t safeRange = std::max<std::int64_t>(1, displayRangeMicros);
  const Segment clipped{
      .startMicros = std::max(startMicros, -safeRange),
      .endMicros = std::min(endMicros, safeRange),
  };
  if (clipped.endMicros <= clipped.startMicros) {
    return std::nullopt;
  }
  return clipped;
}

// Normalized exponential weighting, anchored at the newest input rather than
// wall-clock time. Idle time cannot move the mean or underflow both sums.
class ExponentialAverageAccumulator {
public:
  void add(std::int64_t diffMicros, std::int64_t sampleTimeMicros) noexcept {
    if (totalWeight_ == 0.0) {
      latestSampleTimeMicros_ = sampleTimeMicros;
    }
    if (sampleTimeMicros >= latestSampleTimeMicros_) {
      const double decay = std::exp2(
          (static_cast<double>(latestSampleTimeMicros_) -
           static_cast<double>(sampleTimeMicros)) / kHalfLifeMicros);
      weightedSumMicros_ = weightedSumMicros_ * decay + diffMicros;
      totalWeight_ = totalWeight_ * decay + 1.0;
      latestSampleTimeMicros_ = sampleTimeMicros;
    } else {
      const double weight = std::exp2(
          (static_cast<double>(sampleTimeMicros) -
           static_cast<double>(latestSampleTimeMicros_)) / kHalfLifeMicros);
      weightedSumMicros_ += diffMicros * weight;
      totalWeight_ += weight;
    }
  }

  [[nodiscard]] bool hasSamples() const noexcept { return totalWeight_ > 0.0; }
  [[nodiscard]] std::int64_t latestSampleTimeMicros() const noexcept {
    return latestSampleTimeMicros_;
  }
  [[nodiscard]] std::int64_t value() const noexcept {
    return hasSamples() ? std::llround(weightedSumMicros_ / totalWeight_) : 0;
  }

private:
  static constexpr double kHalfLifeMicros = 1'800'000.0;
  double weightedSumMicros_ = 0.0;
  double totalWeight_ = 0.0;
  std::int64_t latestSampleTimeMicros_ = 0;
};
} // namespace judgement_indicator
