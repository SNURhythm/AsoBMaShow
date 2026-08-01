#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace judgement_indicator {
inline constexpr int kDefaultRangeMilliseconds = 180;
inline constexpr int kMinRangeMilliseconds = 1;
inline constexpr int kMaxRangeMilliseconds = 1000;

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

class RawAverageAccumulator {
public:
  void add(std::int64_t diffMicros) noexcept {
    sumMicros_ += diffMicros;
    ++count_;
  }

  [[nodiscard]] std::size_t count() const noexcept { return count_; }

  [[nodiscard]] std::int64_t value() const noexcept {
    return count_ == 0 ? 0
                       : sumMicros_ / static_cast<std::int64_t>(count_);
  }

private:
  std::int64_t sumMicros_ = 0;
  std::size_t count_ = 0;
};
} // namespace judgement_indicator
