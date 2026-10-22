#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace settings_scene {
inline constexpr long long kCalibrationBeatMicros = 500'000;
inline constexpr long long kCalibrationFirstClickMicros = 1'000'000;
inline constexpr long long kCalibrationFirstNoteMicros = 3'000'000;
inline constexpr int kCalibrationBeats = 64;
inline constexpr long long kCalibrationLoopMicros = 35'000'000;
struct CalibrationTiming {
  long long beatMicros;
  long long firstClickMicros;
  long long firstNoteMicros;
  long long loopMicros;
};
inline CalibrationTiming calibrationTiming(int bpm = 120, int beatsPerMeasure = 4) {
  const auto beat = 60'000'000LL / std::clamp(bpm, 40, 300);
  const auto firstNote = (2 + (beatsPerMeasure == 3 ? 3 : 4)) * beat;
  return {beat, 2 * beat, firstNote, firstNote + kCalibrationBeats * beat};
}

// Measure against the unadjusted audio timeline: gameplay = raw + audio offset.
// One observation per beat also rejects chords and touch/keyboard duplicates.
class OffsetCalibration {
public:
  struct Estimate { int offsetMs; int spreadMs; };
  static constexpr int sampleCount = 16;
  void configure(int bpm, int beatsPerMeasure) { timing_ = calibrationTiming(bpm, beatsPerMeasure); reset(); }
  bool tap(long long rawTimeMicros) {
    const auto window = std::min(200'000LL, timing_.beatMicros * 2 / 5);
    if (count_ == sampleCount || rawTimeMicros < timing_.firstNoteMicros - window)
      return false;
    const auto beat = (rawTimeMicros - timing_.firstNoteMicros + timing_.beatMicros / 2) /
                      timing_.beatMicros;
    if (beat <= lastBeat_ || beat >= kCalibrationBeats) return false;
    const auto error = rawTimeMicros - (timing_.firstNoteMicros + beat * timing_.beatMicros);
    if (std::abs(error) > window) return false;
    lastBeat_ = beat;
    errors_[count_++] = error;
    return true;
  }
  void reset() { count_ = 0; lastBeat_ = -1; }
  // A new chart loop retains the measurements but starts a fresh beat identity.
  void beginLoop() { lastBeat_ = -1; }
  int count() const { return count_; }
  std::optional<Estimate> estimate() const {
    if (count_ < sampleCount) return std::nullopt;
    auto sorted = errors_;
    std::ranges::sort(sorted);
    const auto median = (sorted[7] + sorted[8]) / 2;
    std::array<long long, sampleCount> deviations{};
    std::ranges::transform(sorted, deviations.begin(), [median](auto value) {
      return std::abs(value - median);
    });
    std::ranges::sort(deviations);
    return Estimate{static_cast<int>(std::lround(-median / 1000.0)),
                    static_cast<int>(std::lround((deviations[7] + deviations[8]) / 2000.0))};
  }
private:
  CalibrationTiming timing_ = calibrationTiming();
  std::array<long long, sampleCount> errors_{};
  int count_ = 0;
  long long lastBeat_ = -1;
};
} // namespace settings_scene
