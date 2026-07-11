#pragma once

#include "../ReplayData.h"

#include <cstddef>
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
};

struct Analysis {
  TimingStatistics overall;
  std::vector<HistogramBin> histogram;
  std::vector<LaneAnalysis> lanes;
  std::vector<SectionAnalysis> sections;
};

struct TimingConditions {
  audio::PlaybackRate playback;
  int judgeWindowScalePercent = 100;
  std::vector<JudgeWindowProvenance> effectiveJudgeWindows;

  bool operator==(const TimingConditions &) const = default;
};

struct AnalysisGroup {
  TimingConditions conditions;
  std::vector<std::size_t> attemptIndices;
  Analysis aggregate;
};

[[nodiscard]] Analysis analyze(const bms_parser::Chart &chart,
                               const ReplayData &replay);

[[nodiscard]] std::vector<AnalysisGroup>
analyzeCompatibleAttempts(const bms_parser::Chart &chart,
                          std::span<const ReplayData> attempts);

} // namespace practice
