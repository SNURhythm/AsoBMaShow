#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

// A value-only transcription of the fixed Hi-Speed state owned by pinned
// Beatoraja's LaneRenderer.  It intentionally does not know about input,
// rendering, or persistence, so live play, replay watch, and video export can
// use one authoritative transition model.
namespace gameplay_hispeed {

enum class FixMode : std::uint8_t {
  Off = 0,
  Start = 1,
  Max = 2,
  Main = 3,
  Min = 4,
};

[[nodiscard]] inline constexpr FixMode fixModeFromEncoded(int encoded) {
  switch (encoded) {
  case static_cast<int>(FixMode::Off):
    return FixMode::Off;
  case static_cast<int>(FixMode::Start):
    return FixMode::Start;
  case static_cast<int>(FixMode::Max):
    return FixMode::Max;
  case static_cast<int>(FixMode::Main):
    return FixMode::Main;
  case static_cast<int>(FixMode::Min):
    return FixMode::Min;
  default:
    return FixMode::Main;
  }
}

struct ChartBpmSummary {
  double start = 0.0;
  double minimum = 0.0;
  double maximum = 0.0;
  // LaneRenderer's MAIN value: the BPM with the greatest sum of
  // TimeLine.getTotalNotes(), not the BPM that occupies the longest time.
  double main = 0.0;
};

struct Settings {
  FixMode mode = FixMode::Main;
  int durationMilliseconds = 500;
  float hispeed = 1.0F;
  float margin = 0.25F;
  int laneCoverPercent = 0;
  bool laneCoverEnabled = true;
};

[[nodiscard]] inline constexpr bool isFixed(FixMode mode) noexcept {
  return mode != FixMode::Off;
}

[[nodiscard]] inline double referenceBpm(FixMode mode,
                                          const ChartBpmSummary &summary) {
  switch (mode) {
  case FixMode::Start:
    return summary.start;
  case FixMode::Max:
    return summary.maximum;
  case FixMode::Main:
    return summary.main;
  case FixMode::Min:
    return summary.minimum;
  case FixMode::Off:
    break;
  }
  return 0.0;
}

// LaneRenderer's frame-local `currentduration` computation. `scrollRate` is
// TimeLine.getScroll(); BMS speed objects are outside the parser's present
// visual model and must not be faked here.
[[nodiscard]] inline std::optional<double>
liveDurationValue(double bpm, float hispeed, int laneCoverPercent,
                  bool laneCoverEnabled, double scrollRate = 1.0) {
  if (!std::isfinite(bpm) || bpm <= 0.0 ||
      !std::isfinite(static_cast<double>(hispeed)) || hispeed < 0.0F ||
      !std::isfinite(scrollRate)) {
    return std::nullopt;
  }
  if (scrollRate <= 0.0) {
    return 0.0;
  }
  const double cover = laneCoverEnabled
                           ? static_cast<double>(
                                 std::clamp(laneCoverPercent, 0, 100)) /
                                 100.0
                           : 0.0;
  const double duration =
      240000.0 / bpm / static_cast<double>(hispeed) / scrollRate *
      (1.0 - cover);
  if (duration < 0.0) {
    return std::nullopt;
  }
  return duration;
}

// Java's `(int) Math.round(double)` is a narrowing conversion from the long
// result of Math.round. Keep that exact behavior defined in C++ for the
// zero-Hi-Speed infinities and large finite values LaneRenderer permits.
[[nodiscard]] inline int javaRoundToInt(double value) noexcept {
  if (std::isnan(value)) {
    return 0;
  }
  if (value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return std::bit_cast<std::int32_t>(
        static_cast<std::uint32_t>(std::numeric_limits<std::int64_t>::max()));
  }
  if (value <= static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
    return std::bit_cast<std::int32_t>(
        static_cast<std::uint32_t>(std::numeric_limits<std::int64_t>::min()));
  }
  return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(
      static_cast<std::int64_t>(std::floor(value + 0.5))));
}

[[nodiscard]] inline std::optional<int>
liveDurationMilliseconds(double bpm, float hispeed, int laneCoverPercent,
                         bool laneCoverEnabled, double scrollRate = 1.0) {
  const auto duration = liveDurationValue(bpm, hispeed, laneCoverPercent,
                                          laneCoverEnabled, scrollRate);
  if (!duration) {
    return std::nullopt;
  }
  return javaRoundToInt(*duration);
}

[[nodiscard]] inline int durationToGreenNumber(int duration) noexcept {
  // IntegerPropertyFactory computes `currentduration * 3 / 5` in Java int
  // arithmetic. Use unsigned multiplication to make that deliberate wrap
  // defined instead of relying on signed-overflow behavior in C++.
  const auto product = static_cast<std::uint32_t>(duration) * UINT32_C(3);
  return std::bit_cast<std::int32_t>(product) / 5;
}

class State final {
public:
  State(Settings settings, ChartBpmSummary summary)
      : settings_(sanitize(settings)), summary_(summary),
        baseBpm_(referenceBpm(settings_.mode, summary_)) {
    // LaneRenderer.init: setLanecover() before basehispeed assignment.
    resetHispeed(baseBpm_);
    if (isFixed(settings_.mode)) {
      baseHispeed_ = settings_.hispeed;
    }
  }

  [[nodiscard]] const Settings &settings() const noexcept { return settings_; }
  [[nodiscard]] float hispeed() const noexcept { return settings_.hispeed; }
  [[nodiscard]] float baseHispeed() const noexcept { return baseHispeed_; }
  [[nodiscard]] double baseBpm() const noexcept { return baseBpm_; }

  void setDurationMilliseconds(int milliseconds) {
    // LaneRenderer.setDuration: bound only at one millisecond, then invoke
    // setLanecover() which resets against the selected fixed BPM.
    settings_.durationMilliseconds = std::max(1, milliseconds);
    setLaneCover(settings_.laneCoverPercent);
  }

  void setLaneCover(int percent) {
    settings_.laneCoverPercent = std::clamp(percent, 0, 100);
    resetHispeed(baseBpm_);
  }

  void setLaneCover(int percent, double currentBpm, bool autoAdjust) {
    setLaneCover(percent);
    // ControlInputProcessor.setCoverValue: fixed-speed reset to the current
    // BPM occurs *after* the ordinary setLanecover(basebpm) reset.
    if (autoAdjust && currentBpm > 0.0 && std::isfinite(currentBpm)) {
      resetHispeed(currentBpm);
    }
  }

  void setLaneCoverEnabled(bool enabled) noexcept {
    // LaneRenderer.setEnableLanecover changes only the flag.
    settings_.laneCoverEnabled = enabled;
  }

  void changeHispeed(bool increase) noexcept {
    const float delta =
        (isFixed(settings_.mode) ? baseHispeed_ * settings_.margin
                                 : settings_.margin) *
        (increase ? 1.0F : -1.0F);
    const float candidate = settings_.hispeed + delta;
    // LaneRenderer deliberately rejects, rather than clamps, the endpoints.
    if (candidate > 0.0F && candidate < 20.0F) {
      settings_.hispeed = candidate;
    }
  }

  void resetHispeed(double targetBpm) noexcept {
    if (!isFixed(settings_.mode) || !std::isfinite(targetBpm) ||
        targetBpm <= 0.0) {
      return;
    }
    const double cover = settings_.laneCoverEnabled
                             ? static_cast<double>(settings_.laneCoverPercent) /
                                   100.0
                             : 0.0;
    settings_.hispeed = static_cast<float>(
        240000.0 / targetBpm / settings_.durationMilliseconds *
        (1.0 - cover));
  }

private:
  [[nodiscard]] static Settings sanitize(Settings settings) noexcept {
    settings.durationMilliseconds = std::max(1, settings.durationMilliseconds);
    settings.laneCoverPercent = std::clamp(settings.laneCoverPercent, 0, 100);
    if (!std::isfinite(settings.hispeed)) {
      settings.hispeed = 1.0F;
    }
    if (!std::isfinite(settings.margin)) {
      settings.margin = 0.25F;
    }
    return settings;
  }

  Settings settings_;
  ChartBpmSummary summary_;
  double baseBpm_ = 0.0;
  float baseHispeed_ = 0.0F;
};

} // namespace gameplay_hispeed
