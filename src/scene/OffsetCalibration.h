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
inline constexpr int kCalibrationReferenceBeats = 4;
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

// Measure against the unadjusted audio timeline: gameplay = raw + audio offset.
// One observation per scheduled group rejects chords and input duplicates.
class OffsetCalibration {
public:
  struct Estimate { int offsetMs; int spreadMs; };
  static constexpr int sampleCount = 16;
  void configure(int bpm, int beatsPerMeasure, int subdivision = 4) {
    timing_ = calibrationTiming(bpm, beatsPerMeasure);
    notesPerBeat_ = calibrationNotesPerBeat(subdivision);
    reset();
  }
  bool tap(long long rawTimeMicros) {
    const auto beatWindow = std::min(200'000LL, timing_.beatMicros * 2 / 5);
    if (count_ == sampleCount || rawTimeMicros < timing_.firstNoteMicros - beatWindow)
      return false;
    long long group;
    long long reference = 0;
    auto window = beatWindow;
    const auto denseStart = timing_.firstNoteMicros + kCalibrationReferenceBeats * timing_.beatMicros;
    const bool densePhase = notesPerBeat_ > 1 && rawTimeMicros >= denseStart - beatWindow;
    if (densePhase) {
      // Quarter-note lead-in samples establish a robust reference before
      // dense notes, whose periodic grid alone cannot identify absolute latency.
      if (count_ < kCalibrationReferenceBeats - 1) return false;
      reference = referenceMicros_;
      window = std::min(200'000LL, timing_.beatMicros * 2 / (5 * notesPerBeat_));
      group = std::llround(static_cast<double>(rawTimeMicros - timing_.firstNoteMicros - reference) *
                           notesPerBeat_ / timing_.beatMicros);
    } else {
      // The lead-in and the 1/4 pattern use quarter-note boundaries.
      group = ((rawTimeMicros - timing_.firstNoteMicros + timing_.beatMicros / 2) /
               timing_.beatMicros) * notesPerBeat_;
    }
    if (group <= lastGroup_ || group >= kCalibrationBeats * notesPerBeat_ ||
        (densePhase && group < kCalibrationReferenceBeats * notesPerBeat_)) return false;
    const auto error = rawTimeMicros -
        (timing_.firstNoteMicros + group * timing_.beatMicros / notesPerBeat_);
    if (std::abs(error - reference) > window) return false;
    lastGroup_ = group;
    errors_[count_++] = error;
    if (notesPerBeat_ > 1 && !densePhase && count_ <= kCalibrationReferenceBeats &&
        count_ >= kCalibrationReferenceBeats - 1) {
      // Three samples still tolerate one outlier if a lead-in note was missed.
      std::array<long long, kCalibrationReferenceBeats> referenceErrors{};
      std::copy_n(errors_.begin(), count_, referenceErrors.begin());
      std::sort(referenceErrors.begin(), referenceErrors.begin() + count_);
      referenceMicros_ = (referenceErrors[(count_ - 1) / 2] + referenceErrors[count_ / 2]) / 2;
    }
    return true;
  }
  bool tapDisplay(long long rawTimeMicros, int audioOffsetMs,
                  int visualOffsetMs, int noteDisplayTimingMs) {
    // Match the note's drawn crossing, not its audio/gameplay crossing.
    return tap(rawTimeMicros + (static_cast<long long>(audioOffsetMs) -
                               visualOffsetMs + noteDisplayTimingMs) * 1000);
  }
  void reset() { count_ = 0; lastGroup_ = -1; }
  // A new chart loop retains the measurements but starts a fresh group identity.
  void beginLoop() { lastGroup_ = -1; }
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
  std::optional<Estimate> displayEstimate(int noteDisplayTimingMs) const {
    auto result = estimate();
    // The measured visual delay is independent of the trial offset. Keep the
    // existing note-display timing when expressing its absolute replacement.
    if (result) result->offsetMs += noteDisplayTimingMs;
    return result;
  }
private:
  CalibrationTiming timing_ = calibrationTiming();
  std::array<long long, sampleCount> errors_{};
  long long referenceMicros_ = 0;
  int notesPerBeat_ = 1;
  int count_ = 0;
  long long lastGroup_ = -1;
};
} // namespace settings_scene
