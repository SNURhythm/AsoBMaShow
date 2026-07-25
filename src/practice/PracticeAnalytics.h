#pragma once

#include "../analysis/JudgedPlaybackData.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace practice {

struct TimingStatistics {
  std::size_t samples = 0;
  std::size_t early = 0;
  std::size_t late = 0;
  std::size_t misses = 0;
  std::optional<double> meanMillis;
  std::optional<double> standardDeviationMillis;
  std::optional<double> medianMillis;
};

struct HistogramBin {
  int lowerMillis;
  int upperMillis;
  std::size_t count;
};

struct LaneAnalysis {
  int lane;
  TimingStatistics timing;
};

struct SectionAnalysis {
  int firstMeasure;
  int lastMeasure;
  long long startMicros;
  long long endMicros;
  TimingStatistics timing;
  double badMissRate = 0.0;
  std::optional<double> accuracy;
};

struct Analysis {
  TimingStatistics overall;
  std::vector<HistogramBin> histogram;
  // Samples whose 5 ms bin bounds cannot fit HistogramBin's int endpoints.
  std::size_t histogramLowerOverflow = 0;
  std::size_t histogramUpperOverflow = 0;
  std::vector<LaneAnalysis> lanes;
  std::vector<SectionAnalysis> sections;
};

enum class TimingWindowResolution : std::uint8_t {
  LegacyAbsent,
  Resolved,
  Unresolved,
};

struct TimingConditions {
  audio::PlaybackRate playback;
  int judgeWindowScalePercent = 100;
  TimingWindowResolution windowResolution =
      TimingWindowResolution::LegacyAbsent;
  std::vector<analysis::JudgedWindow> effectiveJudgeWindows;

  bool operator==(const TimingConditions &) const = default;
};

struct AnalysisGroup {
  TimingConditions conditions;
  std::vector<std::size_t> attemptIndices;
  Analysis aggregate;
};

[[nodiscard]] Analysis analyze(const bms_parser::Chart &chart,
                               const JudgedPlaybackData &replay);

[[nodiscard]] std::vector<AnalysisGroup>
analyzeCompatibleAttempts(const bms_parser::Chart &chart,
                          std::span<const JudgedPlaybackData> attempts);

} // namespace practice
