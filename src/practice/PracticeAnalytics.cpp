#include "PracticeAnalytics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <utility>

namespace practice {
namespace {

constexpr long long kHistogramBinMicros = 5'000;
constexpr int kLowestFiniteHistogramLower =
    std::numeric_limits<int>::min() - std::numeric_limits<int>::min() % 5;
constexpr int kHighestFiniteHistogramLower =
    (std::numeric_limits<int>::max() - 5) / 5 * 5;

struct NoteIdentity {
  int lane;
  long long timingMicros;

  bool operator<(const NoteIdentity &other) const {
    return std::tie(lane, timingMicros) <
           std::tie(other.lane, other.timingMicros);
  }
};

struct Sample {
  long long realMicros;
  Judgement judgement;
};

struct Accumulator {
  std::vector<Sample> samples;
  std::size_t misses = 0;
  std::size_t bad = 0;
};

struct ChartLayout {
  std::map<NoteIdentity, std::size_t> noteSections;
  std::vector<SectionAnalysis> sections;
};

long long floorDivide(long long value, long long divisor) {
  const long long quotient = value / divisor;
  const long long remainder = value % divisor;
  return remainder < 0 ? quotient - 1 : quotient;
}

TimingStatistics summarize(const Accumulator &accumulator) {
  TimingStatistics result;
  result.samples = accumulator.samples.size();
  result.misses = accumulator.misses;
  for (const auto &sample : accumulator.samples) {
    result.early += sample.realMicros < 0 ? 1 : 0;
    result.late += sample.realMicros > 0 ? 1 : 0;
  }
  if (accumulator.samples.empty()) {
    return result;
  }

  std::vector<long long> sorted;
  sorted.reserve(accumulator.samples.size());
  long double sum = 0.0L;
  for (const auto &sample : accumulator.samples) {
    sorted.push_back(sample.realMicros);
    sum += static_cast<long double>(sample.realMicros);
  }
  const long double meanMicros =
      sum / static_cast<long double>(accumulator.samples.size());
  long double squaredDifferenceSum = 0.0L;
  for (const auto &sample : accumulator.samples) {
    const long double difference =
        static_cast<long double>(sample.realMicros) - meanMicros;
    squaredDifferenceSum += difference * difference;
  }
  std::sort(sorted.begin(), sorted.end());
  const std::size_t middle = sorted.size() / 2;
  const long double medianMicros =
      sorted.size() % 2 == 0 ? (static_cast<long double>(sorted[middle - 1]) +
                                static_cast<long double>(sorted[middle])) /
                                   2.0L
                             : static_cast<long double>(sorted[middle]);

  result.meanMillis = static_cast<double>(meanMicros / 1'000.0L);
  result.standardDeviationMillis = static_cast<double>(
      std::sqrt(squaredDifferenceSum /
                static_cast<long double>(accumulator.samples.size())) /
      1'000.0L);
  result.medianMillis = static_cast<double>(medianMicros / 1'000.0L);
  return result;
}

double accuracyFor(const Accumulator &accumulator) {
  const std::size_t opportunities =
      accumulator.samples.size() + accumulator.misses;
  if (opportunities == 0) {
    return 0.0;
  }
  std::size_t points = 0;
  for (const auto &sample : accumulator.samples) {
    if (sample.judgement == PGreat) {
      points += 2;
    } else if (sample.judgement == Great) {
      points += 1;
    }
  }
  return static_cast<double>(points) /
         static_cast<double>(opportunities * 2);
}

long long chartEndMicros(const bms_parser::Chart &chart) {
  long long result = std::max(chart.Meta.PlayLength, chart.Meta.TotalLength);
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    result = std::max(result, measure->Timing);
    for (const auto *timeline : measure->TimeLines) {
      if (timeline != nullptr) {
        result = std::max(result, timeline->Timing);
      }
    }
  }
  return result;
}

ChartLayout buildChartLayout(const bms_parser::Chart &chart) {
  ChartLayout result;
  const long long chartEnd = chartEndMicros(chart);
  std::vector<std::pair<std::size_t, const bms_parser::Measure *>> measures;
  for (std::size_t index = 0; index < chart.Measures.size(); ++index) {
    if (chart.Measures[index] != nullptr) {
      measures.emplace_back(index, chart.Measures[index]);
    }
  }

  result.sections.reserve(measures.size());
  for (std::size_t sectionIndex = 0; sectionIndex < measures.size();
       ++sectionIndex) {
    const auto [measureIndex, measure] = measures[sectionIndex];
    const long long endMicros = sectionIndex + 1 < measures.size()
                                    ? measures[sectionIndex + 1].second->Timing
                                    : std::max(chartEnd, measure->Timing);
    result.sections.push_back({
        .firstMeasure = static_cast<int>(measureIndex),
        .lastMeasure = static_cast<int>(measureIndex),
        .startMicros = measure->Timing,
        .endMicros = endMicros,
    });

    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (const auto *note : timeline->Notes) {
        if (note == nullptr ||
            dynamic_cast<const bms_parser::LandmineNote *>(note) != nullptr) {
          continue;
        }
        result.noteSections.try_emplace(
            NoteIdentity{.lane = note->Lane, .timingMicros = timeline->Timing},
            sectionIndex);
      }
    }
  }
  return result;
}

bool isTimingEvent(const ReplayEvent &event) {
  return (event.action == ReplayEventAction::Press ||
          event.action == ReplayEventAction::MultiBad ||
          event.action == ReplayEventAction::Release) &&
         event.judgement != None && event.judgement != Kpoor;
}

std::optional<std::size_t> sectionForEvent(const ChartLayout &layout,
                                           const ReplayEvent &event) {
  if (event.noteTimeMicros < 0) {
    return std::nullopt;
  }
  const auto found = layout.noteSections.find(
      {.lane = event.lane, .timingMicros = event.noteTimeMicros});
  if (found == layout.noteSections.end()) {
    return std::nullopt;
  }
  return found->second;
}

TimingConditions conditionsFor(const bms_parser::Chart &chart,
                               const JudgedPlaybackData &replay) {
  TimingConditions result{
      .playback = {.percent = replay.setup.playbackRatePercent,
                   .mode = replay.setup.playbackMode},
      .judgeWindowScalePercent = replay.setup.judgeWindowScalePercent,
  };
  if (!replay.context.policy.has_value()) {
    if (replay.context.ruleset.version > 0) {
      result.windowResolution = TimingWindowResolution::Unresolved;
    }
    return result;
  }

  const auto &policy = *replay.context.policy;
  if (!course_identity::sameChart(
          {.sha256 = policy.chartSha256, .md5 = policy.chartMd5},
          {.sha256 = chart.Meta.SHA256, .md5 = chart.Meta.MD5})) {
    result.windowResolution = TimingWindowResolution::Unresolved;
    return result;
  }
  result.windowResolution = TimingWindowResolution::Resolved;
  result.effectiveJudgeWindows = policy.effectiveJudgeWindows;
  return result;
}

Analysis analyzeAttempts(const bms_parser::Chart &chart,
                         std::span<const JudgedPlaybackData *const> attempts,
                         const audio::PlaybackRate &playback) {
  const ChartLayout layout = buildChartLayout(chart);
  Accumulator overall;
  std::map<int, Accumulator> lanes;
  std::vector<Accumulator> sections(layout.sections.size());

  for (const JudgedPlaybackData *replay : attempts) {
    if (replay == nullptr) {
      continue;
    }
    for (const auto &event : replay->events) {
      const auto section = sectionForEvent(layout, event);
      if (!section.has_value()) {
        continue;
      }
      if (event.action == ReplayEventAction::Miss && event.judgement == Poor) {
        ++overall.misses;
        ++lanes[event.lane].misses;
        ++sections[*section].misses;
        continue;
      }
      if (!isTimingEvent(event)) {
        continue;
      }

      const Sample sample{
          .realMicros = playback.realMicrosFromChart(event.diffMicros),
          .judgement = event.judgement,
      };
      overall.samples.push_back(sample);
      lanes[event.lane].samples.push_back(sample);
      sections[*section].samples.push_back(sample);
      if (event.judgement == Bad) {
        ++overall.bad;
        ++lanes[event.lane].bad;
        ++sections[*section].bad;
      }
    }
  }

  Analysis result;
  result.overall = summarize(overall);
  if (!overall.samples.empty()) {
    std::map<int, std::size_t> binCounts;
    for (const auto &sample : overall.samples) {
      const long long lower =
          floorDivide(sample.realMicros, kHistogramBinMicros) * 5;
      if (lower < kLowestFiniteHistogramLower) {
        ++result.histogramLowerOverflow;
      } else if (lower > kHighestFiniteHistogramLower) {
        ++result.histogramUpperOverflow;
      } else {
        ++binCounts[static_cast<int>(lower)];
      }
    }
    result.histogram.reserve(binCounts.size());
    for (const auto &[lower, count] : binCounts) {
      result.histogram.push_back(
          {.lowerMillis = lower, .upperMillis = lower + 5, .count = count});
    }
  }

  result.lanes.reserve(lanes.size());
  for (const auto &[lane, accumulator] : lanes) {
    result.lanes.push_back({.lane = lane, .timing = summarize(accumulator)});
  }

  result.sections = layout.sections;
  for (std::size_t index = 0; index < result.sections.size(); ++index) {
    auto &section = result.sections[index];
    const auto &accumulator = sections[index];
    section.timing = summarize(accumulator);
    const std::size_t opportunities =
        accumulator.samples.size() + accumulator.misses;
    section.badMissRate =
        opportunities == 0
            ? 0.0
            : static_cast<double>(accumulator.bad + accumulator.misses) /
                  static_cast<double>(opportunities);
    if (opportunities > 0) {
      section.accuracy = accuracyFor(accumulator);
    }
  }
  return result;
}

} // namespace

Analysis analyze(const bms_parser::Chart &chart, const JudgedPlaybackData &replay) {
  const JudgedPlaybackData *attempt = &replay;
  return analyzeAttempts(
      chart, std::span(&attempt, 1),
      {.percent = replay.setup.playbackRatePercent,
       .mode = replay.setup.playbackMode});
}

std::vector<AnalysisGroup>
analyzeCompatibleAttempts(const bms_parser::Chart &chart,
                          std::span<const JudgedPlaybackData> attempts) {
  std::vector<AnalysisGroup> result;
  std::vector<std::vector<const JudgedPlaybackData *>> groupedAttempts;
  for (std::size_t index = 0; index < attempts.size(); ++index) {
    const TimingConditions conditions = conditionsFor(chart, attempts[index]);
    const auto found =
        conditions.windowResolution == TimingWindowResolution::Unresolved
            ? result.end()
            : std::find_if(result.begin(), result.end(),
                           [&](const AnalysisGroup &group) {
                             return group.conditions == conditions;
                           });
    if (found == result.end()) {
      result.push_back({.conditions = conditions, .attemptIndices = {index}});
      groupedAttempts.push_back({&attempts[index]});
      continue;
    }
    const std::size_t groupIndex =
        static_cast<std::size_t>(std::distance(result.begin(), found));
    found->attemptIndices.push_back(index);
    groupedAttempts[groupIndex].push_back(&attempts[index]);
  }

  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index].aggregate = analyzeAttempts(
        chart, groupedAttempts[index], result[index].conditions.playback);
  }
  return result;
}

} // namespace practice
