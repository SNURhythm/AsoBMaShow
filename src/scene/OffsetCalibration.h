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

inline int calibrationNotesPerBeat(int subdivision) {
  switch (subdivision) {
    case 8: case 16: case 24: case 32: return subdivision / 4;
    default: return 1;
  }
}

// A perfectly timed note is at gameplay time, while its drawing uses
// gameplay - visual offset + note-display timing. Scroll is measured in bars.
inline double calibrationLogicalJudgementScrollDelta(int visualOffsetMs,
                                                      int noteDisplayTimingMs,
                                                      double bpm) {
  return (visualOffsetMs - noteDisplayTimingMs) * bpm / 240'000.0;
}

// Display judging follows the note's drawn crossing on the gameplay timeline.
inline long long calibrationDisplayTimelineMicros(long long gameplayTimeMicros,
                                                  int visualOffsetMs,
                                                  int noteDisplayTimingMs) {
  return gameplayTimeMicros + (static_cast<long long>(noteDisplayTimingMs) - visualOffsetMs) * 1000;
}

// Measure the target selected by gameplay judging against the raw audio clock.
// One observation per scheduled group rejects chords and repeated judgements.
class OffsetCalibration {
public:
  struct Estimate { int offsetMs; int spreadMs; };
  void configure(int beatsPerMeasure, int subdivision = 4) {
    requiredCount_ = 4 * (beatsPerMeasure == 3 ? 3 : 4) * calibrationNotesPerBeat(subdivision);
    reset();
  }
  bool tap(long long rawTimeMicros, long long noteTimeMicros) {
    if (count_ == requiredCount_ ||
        std::find(noteTimes_.begin() + loopStart_, noteTimes_.begin() + count_, noteTimeMicros) !=
            noteTimes_.begin() + count_) return false;
    noteTimes_[count_] = noteTimeMicros;
    errors_[count_++] = rawTimeMicros - noteTimeMicros;
    return true;
  }
  bool tapDisplay(long long rawTimeMicros, long long noteTimeMicros, int audioOffsetMs,
                  int visualOffsetMs, int noteDisplayTimingMs) {
    return tap(calibrationDisplayTimelineMicros(rawTimeMicros + audioOffsetMs * 1000LL,
                                                visualOffsetMs, noteDisplayTimingMs), noteTimeMicros);
  }
  void reset() { count_ = 0; loopStart_ = 0; }
  // A new chart loop retains measurements and permits those note groups again.
  void beginLoop() { loopStart_ = count_; }
  int count() const { return count_; }
  int requiredCount() const { return requiredCount_; }
  std::optional<Estimate> estimate() const {
    if (count_ < requiredCount_) return std::nullopt;
    auto sorted = errors_;
    std::sort(sorted.begin(), sorted.begin() + count_);
    const auto median = (sorted[count_ / 2 - 1] + sorted[count_ / 2]) / 2;
    std::array<long long, maxSampleCount> deviations{};
    std::transform(sorted.begin(), sorted.begin() + count_, deviations.begin(), [median](auto value) {
      return std::abs(value - median);
    });
    std::sort(deviations.begin(), deviations.begin() + count_);
    return Estimate{static_cast<int>(std::lround(-median / 1000.0)),
                    static_cast<int>(std::lround((deviations[count_ / 2 - 1] +
                                                 deviations[count_ / 2]) / 2000.0))};
  }
  std::optional<Estimate> displayEstimate(int noteDisplayTimingMs) const {
    auto result = estimate();
    // The measured visual delay is independent of the trial offset. Keep the
    // existing note-display timing when expressing its absolute replacement.
    if (result) result->offsetMs += noteDisplayTimingMs;
    return result;
  }
private:
  static constexpr int maxSampleCount = 4 * 4 * 8;
  std::array<long long, maxSampleCount> errors_{};
  std::array<long long, maxSampleCount> noteTimes_{};
  int requiredCount_ = 16;
  int count_ = 0;
  int loopStart_ = 0;
};
} // namespace settings_scene
