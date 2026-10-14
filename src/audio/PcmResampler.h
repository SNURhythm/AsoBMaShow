#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>

namespace audio::pcm_resampling {

// Positive integer rates may include playback-speed factors. Reduce once so
// the frame loop only advances an exact integer phase, including on Android
// where long-double arithmetic is implemented in software.
class Ratio {
public:
  Ratio(std::int64_t sourceRate, std::int64_t targetRate) noexcept {
    const auto divisor = std::gcd(sourceRate, targetRate);
    numerator = sourceRate / divisor;
    denominator = targetRate / divisor;
  }

  [[nodiscard]] bool direct() const noexcept { return numerator == denominator; }

  [[nodiscard]] std::optional<std::size_t>
  outputFrames(std::size_t sourceFrames, std::size_t maximumFrames) const noexcept {
    const auto source = static_cast<std::uint64_t>(numerator);
    const auto target = static_cast<std::uint64_t>(denominator);
    const auto whole = sourceFrames / source;
    const auto remainder = sourceFrames % source;
    if (whole > maximumFrames / target ||
        remainder > std::numeric_limits<std::uint64_t>::max() / target)
      return std::nullopt;
    const auto fractionalNumerator = remainder * target;
    const auto fractional = fractionalNumerator / source +
                            (fractionalNumerator % source != 0);
    const auto wholeFrames = whole * target;
    if (fractional > maximumFrames - wholeFrames) return std::nullopt;
    return static_cast<std::size_t>(wholeFrames + fractional);
  }

  std::int64_t numerator;
  std::int64_t denominator;
};

// Requires at least one source frame. Both consumers validate decoded PCM
// before constructing a cursor, so interpolation needs no per-sample bounds check.
class Cursor {
public:
  Cursor(const Ratio &ratio, std::size_t sourceFrames) noexcept
      : lastFrame(sourceFrames - 1), denominator(ratio.denominator),
        wholeStep(static_cast<std::size_t>(ratio.numerator / ratio.denominator)),
        fractionalStep(ratio.numerator % ratio.denominator) {}

  [[nodiscard]] std::size_t left() const noexcept { return leftFrame; }
  [[nodiscard]] std::size_t right() const noexcept {
    return std::min(leftFrame + 1, lastFrame);
  }

  [[nodiscard]] short interpolate(short leftSample, short rightSample) const noexcept {
    // Convex interpolation stays in int16 range. Supported sample rates and
    // playback factors also keep this weighted numerator within int64.
    const std::int64_t numerator =
        leftSample * (denominator - fraction) + rightSample * fraction;
    return static_cast<short>(
        (numerator + (numerator >= 0 ? denominator / 2 : -denominator / 2)) /
        denominator);
  }

  void advance() noexcept {
    leftFrame += std::min(wholeStep, lastFrame - leftFrame);
    fraction += fractionalStep;
    if (fraction >= denominator) {
      fraction -= denominator;
      if (leftFrame < lastFrame) ++leftFrame;
    }
  }

private:
  const std::size_t lastFrame;
  const std::int64_t denominator;
  const std::size_t wholeStep;
  const std::int64_t fractionalStep;
  std::size_t leftFrame = 0;
  std::int64_t fraction = 0;
};

} // namespace audio::pcm_resampling
