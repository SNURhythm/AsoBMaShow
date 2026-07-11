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

int binLowerMillis(long long realMicros) {
  const long long lower = floorDivide(realMicros, kHistogramBinMicros) * 5;
  return static_cast<int>(
      std::clamp(lower, static_cast<long long>(std::numeric_limits<int>::min()),
                 static_cast<long long>(std::numeric_limits<int>::max() - 5)));
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

std::vector<JudgeWindowProvenance>
effectiveWindows(const bms_parser::Chart &chart, const ReplayData &replay) {
  if (replay.provenance.stages.size() == 1) {
    return replay.provenance.stages.front().effectiveJudgeWindows;
  }

  const ScoreStageProvenance *matching = nullptr;
  for (const auto &stage : replay.provenance.stages) {
    const bool shaMatches =
        !chart.Meta.SHA256.empty() && stage.chartSha256 == chart.Meta.SHA256;
    const bool md5Matches =
        !chart.Meta.MD5.empty() && stage.chartMd5 == chart.Meta.MD5;
    if (!shaMatches && !md5Matches) {
      continue;
    }
    if (matching != nullptr) {
      return {};
    }
    matching = &stage;
  }
  return matching == nullptr ? std::vector<JudgeWindowProvenance>{}
                             : matching->effectiveJudgeWindows;
}

TimingConditions conditionsFor(const bms_parser::Chart &chart,
                               const ReplayData &replay) {
  return {
      .playback = replay.provenance.playback,
      .judgeWindowScalePercent = replay.provenance.judgeWindowScalePercent,
      .effectiveJudgeWindows = effectiveWindows(chart, replay),
  };
}

Analysis analyzeAttempts(const bms_parser::Chart &chart,
                         std::span<const ReplayData *const> attempts,
                         const audio::PlaybackRate &playback) {
  const ChartLayout layout = buildChartLayout(chart);
  Accumulator overall;
  std::map<int, Accumulator> lanes;
  std::vector<Accumulator> sections(layout.sections.size());

  for (const ReplayData *replay : attempts) {
    if (replay == nullptr) {
      continue;
    }
    for (const auto &event : replay->events) {
      const auto section = sectionForEvent(layout, event);
      if (!section.has_value()) {
        continue;
      }
      if (event.action == ReplayEventAction::Miss) {
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
      ++binCounts[binLowerMillis(sample.realMicros)];
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
  }
  return result;
}

} // namespace

Analysis analyze(const bms_parser::Chart &chart, const ReplayData &replay) {
  const ReplayData *attempt = &replay;
  return analyzeAttempts(chart, std::span(&attempt, 1),
                         replay.provenance.playback);
}

std::vector<AnalysisGroup>
analyzeCompatibleAttempts(const bms_parser::Chart &chart,
                          std::span<const ReplayData> attempts) {
  std::vector<AnalysisGroup> result;
  std::vector<std::vector<const ReplayData *>> groupedAttempts;
  for (std::size_t index = 0; index < attempts.size(); ++index) {
    const TimingConditions conditions = conditionsFor(chart, attempts[index]);
    const auto found = std::find_if(result.begin(), result.end(),
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
