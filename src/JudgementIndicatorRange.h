#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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

// Continuous exponential weighting for the running timing average.
[[nodiscard]] inline double timingWeight(std::int64_t sampleTimeMicros,
                                         std::int64_t currentTimeMicros) noexcept {
  constexpr double halfLifeMicros = 1'800'000.0;
  const double ageMicros = std::max(
      0.0, static_cast<double>(currentTimeMicros) - sampleTimeMicros);
  return std::exp2(-ageMicros / halfLifeMicros);
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
      const double decay = timingWeight(latestSampleTimeMicros_, sampleTimeMicros);
      weightedSumMicros_ = weightedSumMicros_ * decay + diffMicros;
      totalWeight_ = totalWeight_ * decay + 1.0;
      latestSampleTimeMicros_ = sampleTimeMicros;
    } else {
      const double weight = timingWeight(sampleTimeMicros, latestSampleTimeMicros_);
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
  double weightedSumMicros_ = 0.0;
  double totalWeight_ = 0.0;
  std::int64_t latestSampleTimeMicros_ = 0;
};

// Evicted bars still contribute to the average. Keep their timestamps for
// frames before the newest input, while ordinary forward frames use the cached
// accumulator. Copies share an append-only journal but retain their own length,
// so capturing a frame is constant-time and later inputs cannot change it.
class TimingAverageHistory {
public:
  void add(std::int64_t diffMicros, std::int64_t sampleTimeMicros) {
    if (!samples_) {
      samples_ = std::make_shared<Journal>();
    }
    const auto journal = samples_;
    const std::lock_guard lock(journal->mutex);
    if (size_ < journal->entries.size() &&
        (journal->entries[size_].diffMicros != diffMicros ||
         journal->entries[size_].timeMicros != sampleTimeMicros)) {
      // A copied frame may start a different timeline. Preserve the original
      // journal for other snapshots and copy only this frame's prefix.
      samples_ = std::make_shared<Journal>();
      samples_->entries.assign(journal->entries.begin(),
                               journal->entries.begin() + size_);
    }
    if (size_ == samples_->entries.size()) {
      samples_->entries.push_back({diffMicros, sampleTimeMicros});
    }
    ++size_;
    average_.add(diffMicros, sampleTimeMicros);
  }

  [[nodiscard]] ExponentialAverageAccumulator
  at(std::int64_t currentTimeMicros) const {
    if (!average_.hasSamples() ||
        average_.latestSampleTimeMicros() <= currentTimeMicros) {
      return average_;
    }
    ExponentialAverageAccumulator result;
    const std::lock_guard lock(samples_->mutex);
    for (std::size_t index = 0; index < size_; ++index) {
      const auto &sample = samples_->entries[index];
      if (sample.timeMicros <= currentTimeMicros) {
        result.add(sample.diffMicros, sample.timeMicros);
      }
    }
    return result;
  }

private:
  struct Sample {
    std::int64_t diffMicros;
    std::int64_t timeMicros;
  };
  struct Journal {
    std::mutex mutex;
    std::vector<Sample> entries;
  };
  std::shared_ptr<Journal> samples_;
  std::size_t size_ = 0;
  ExponentialAverageAccumulator average_;
};
} // namespace judgement_indicator
