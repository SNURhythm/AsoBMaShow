#include "PrepMetronome.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

namespace prep_metronome {
namespace {
constexpr double kDefaultBpm = 120.0;
constexpr int kDefaultBeatsPerMeasure = 4;
constexpr int kMinBeatsPerMeasure = 1;
constexpr int kMaxBeatsPerMeasure = 16;
constexpr double kMinSaneBpm = 30.0;
constexpr double kMaxSaneBpm = 400.0;
constexpr double kMicrosPerMinute = 60000000.0;
constexpr double kMicrosPerBmsMeasure = 240000000.0;
constexpr double kBeatPositionStep = 0.25;
constexpr double kBeatPositionTolerance = 0.000001;

struct ChartBeat {
  long long timeMicros = 0;
  bool accent = false;
};

bool isPositiveBpm(double bpm) {
  return std::isfinite(bpm) && bpm > 0.0;
}
} // namespace

bool isSaneBpm(double bpm) {
  return std::isfinite(bpm) && bpm >= kMinSaneBpm && bpm <= kMaxSaneBpm;
}

double effectiveBpm(const bms_parser::ChartMeta &meta,
                    std::optional<double> firstMeasureBpm) {
  if (isSaneBpm(meta.GuessedBeatBpm)) {
    return meta.GuessedBeatBpm;
  }
  if (firstMeasureBpm.has_value() && isSaneBpm(*firstMeasureBpm)) {
    return *firstMeasureBpm;
  }
  if (isSaneBpm(meta.Bpm)) {
    return meta.Bpm;
  }
  if (std::isfinite(meta.MostPrevalentBpm) && meta.MostPrevalentBpm > 0.0) {
    return meta.MostPrevalentBpm;
  }
  return kDefaultBpm;
}

double effectiveBpm(const bms_parser::ChartMeta &meta) {
  return effectiveBpm(meta, std::nullopt);
}

std::optional<double> firstMeasureBpmCandidate(const bms_parser::Chart &chart) {
  if (chart.Measures.empty() || chart.Measures.front() == nullptr ||
      chart.Measures.front()->TimeLines.empty()) {
    return std::nullopt;
  }

  const auto *timeline = chart.Measures.front()->TimeLines.front();
  if (timeline != nullptr && timeline->BpmChange &&
      std::isfinite(timeline->Bpm) && timeline->Bpm > 0.0) {
    return timeline->Bpm;
  }
  return std::nullopt;
}

long long beatIntervalMicrosForBpm(double bpm) {
  const double interval = kMicrosPerMinute / bpm;
  const auto maxInterval =
      static_cast<double>(std::numeric_limits<long long>::max() /
                          kMaxBeatsPerMeasure);
  if (!std::isfinite(interval) || interval >= maxInterval) {
    return std::numeric_limits<long long>::max() / kMaxBeatsPerMeasure;
  }
  return std::max(1LL, static_cast<long long>(std::llround(interval)));
}

namespace {
struct ChartBeatWalk {
  std::deque<ChartBeat> beats;
  double markerBpm = kDefaultBpm;
  double initialGridBpm = kDefaultBpm;
  std::optional<long long> firstGridTimeMicros;
};

double initialChartBpm(const bms_parser::Chart &chart) {
  return isPositiveBpm(chart.Meta.Bpm)
             ? chart.Meta.Bpm
             : effectiveBpm(chart.Meta, firstMeasureBpmCandidate(chart));
}

ChartBeatWalk walkChartBeatsBefore(const bms_parser::Chart &chart,
                                   long long startMicros,
                                   std::size_t beatLimit) {
  ChartBeatWalk result;
  double activeBpm = initialChartBpm(chart);
  result.markerBpm = activeBpm;
  result.initialGridBpm = activeBpm;
  double measureBeatPosition = 0.0;

  const auto appendBeat = [&result, beatLimit](ChartBeat beat) {
    if (!result.beats.empty() &&
        result.beats.back().timeMicros == beat.timeMicros) {
      result.beats.back().accent = result.beats.back().accent || beat.accent;
      return;
    }
    result.beats.push_back(beat);
    if (result.beats.size() > beatLimit) {
      result.beats.pop_front();
    }
  };

  for (const auto *measure : chart.Measures) {
    if (measure == nullptr || !std::isfinite(measure->Scale) ||
        measure->Scale <= 0.0) {
      continue;
    }

    long long timingCursorMicros = measure->Timing;
    double timingCursorBeatPosition = measureBeatPosition;
    std::size_t timelineIndex = 0;
    const auto processTimeline = [&](const bms_parser::TimeLine &timeline) {
      timingCursorMicros =
          timeline.Timing +
          std::max(0LL, static_cast<long long>(timeline.GetStopDuration()));
      timingCursorBeatPosition = timeline.BeatPosition;
      if (timeline.BpmChange && isPositiveBpm(timeline.Bpm)) {
        activeBpm = timeline.Bpm;
      }
    };

    for (double localBeatPosition = 0.0;
         localBeatPosition < measure->Scale - kBeatPositionTolerance;
         localBeatPosition += kBeatPositionStep) {
      const double targetBeatPosition =
          measureBeatPosition + localBeatPosition;
      while (timelineIndex < measure->TimeLines.size()) {
        const auto *timeline = measure->TimeLines[timelineIndex];
        if (timeline == nullptr ||
            timeline->BeatPosition + kBeatPositionTolerance <
                timingCursorBeatPosition) {
          ++timelineIndex;
          continue;
        }
        if (timeline->BeatPosition + kBeatPositionTolerance >=
            targetBeatPosition) {
          break;
        }
        if (timeline->Timing > startMicros) {
          result.markerBpm = activeBpm;
          return result;
        }
        processTimeline(*timeline);
        ++timelineIndex;
      }

      if (!isPositiveBpm(activeBpm)) {
        activeBpm = kDefaultBpm;
      }
      long long timeMicros = measure->Timing;
      if (localBeatPosition > kBeatPositionTolerance) {
        const double beatDistance =
            targetBeatPosition - timingCursorBeatPosition;
        timeMicros = timingCursorMicros +
                     static_cast<long long>(std::llround(
                         kMicrosPerBmsMeasure * beatDistance / activeBpm));
      }

      const bool firstGridBeat = !result.firstGridTimeMicros.has_value();
      if (firstGridBeat) {
        result.firstGridTimeMicros = timeMicros;
      }

      while (timelineIndex < measure->TimeLines.size()) {
        const auto *timeline = measure->TimeLines[timelineIndex];
        if (timeline == nullptr ||
            timeline->BeatPosition + kBeatPositionTolerance <
                targetBeatPosition) {
          ++timelineIndex;
          continue;
        }
        if (std::abs(timeline->BeatPosition - targetBeatPosition) >
            kBeatPositionTolerance) {
          break;
        }
        if (timeline->Timing <= startMicros) {
          timeMicros = timeline->Timing;
          processTimeline(*timeline);
        }
        ++timelineIndex;
      }

      if (firstGridBeat) {
        result.firstGridTimeMicros = timeMicros;
        result.initialGridBpm = activeBpm;
      }
      if (timeMicros < startMicros) {
        appendBeat({.timeMicros = timeMicros,
                    .accent = localBeatPosition <=
                              kBeatPositionTolerance});
      } else {
        result.markerBpm = activeBpm;
        return result;
      }
    }

    while (timelineIndex < measure->TimeLines.size()) {
      const auto *timeline = measure->TimeLines[timelineIndex++];
      if (timeline == nullptr) {
        continue;
      }
      if (timeline->Timing > startMicros) {
        result.markerBpm = activeBpm;
        return result;
      }
      processTimeline(*timeline);
    }
    measureBeatPosition += measure->Scale;
  }

  result.markerBpm = activeBpm;
  return result;
}
} // namespace

int effectiveBeatsPerMeasure(const bms_parser::ChartMeta &meta) {
  if (meta.GuessedBeatsPerMeasure >= kMinBeatsPerMeasure &&
      meta.GuessedBeatsPerMeasure <= kMaxBeatsPerMeasure) {
    return meta.GuessedBeatsPerMeasure;
  }
  return kDefaultBeatsPerMeasure;
}

PrepMetronomePlan buildPlanFromMeta(
    const bms_parser::ChartMeta &meta, std::optional<double> firstMeasureBpm,
    bool settingEnabled, bool chartPreviewPlayback,
    long long playbackAnchorMicros) {
  PrepMetronomePlan plan;
  if (!settingEnabled || chartPreviewPlayback) {
    return plan;
  }

  plan.enabled = true;
  plan.bpm = effectiveBpm(meta, firstMeasureBpm);
  plan.beatsPerMeasure = effectiveBeatsPerMeasure(meta);
  plan.beatIntervalMicros = beatIntervalMicrosForBpm(plan.bpm);
  plan.leadInMicros =
      plan.beatIntervalMicros * static_cast<long long>(plan.beatsPerMeasure);
  plan.startTimeMicros = playbackAnchorMicros - plan.leadInMicros;

  plan.clicks.reserve(static_cast<size_t>(plan.beatsPerMeasure));
  for (int beat = 0; beat < plan.beatsPerMeasure; ++beat) {
    plan.clicks.push_back(
        {.timeMicros = plan.startTimeMicros +
                       plan.beatIntervalMicros * static_cast<long long>(beat),
         .accent = beat == 0});
  }

  return plan;
}

PrepMetronomePlan buildPlan(const bms_parser::ChartMeta &meta,
                            bool settingEnabled, bool chartPreviewPlayback,
                            long long playbackAnchorMicros) {
  return buildPlanFromMeta(meta, std::nullopt, settingEnabled,
                           chartPreviewPlayback, playbackAnchorMicros);
}

PrepMetronomePlan buildPlan(const bms_parser::Chart &chart,
                            bool settingEnabled, bool chartPreviewPlayback,
                            long long playbackAnchorMicros) {
  return buildPlanFromMeta(chart.Meta, firstMeasureBpmCandidate(chart),
                           settingEnabled, chartPreviewPlayback,
                           playbackAnchorMicros);
}

PrepMetronomePlan buildPracticeCountInPlan(
    const bms_parser::Chart &chart, long long startMicros, int countInBeats,
    audio::PlaybackRate playback) {
  PrepMetronomePlan plan;
  if (countInBeats <= 0) {
    return plan;
  }

  plan.enabled = true;
  auto beatWalk = walkChartBeatsBefore(
      chart, startMicros, static_cast<std::size_t>(countInBeats));
  plan.bpm = beatWalk.markerBpm;
  plan.beatsPerMeasure = effectiveBeatsPerMeasure(chart.Meta);
  plan.beatIntervalMicros = beatIntervalMicrosForBpm(plan.bpm);
  auto &beats = beatWalk.beats;
  const long long initialBeatIntervalMicros =
      beatIntervalMicrosForBpm(beatWalk.initialGridBpm);
  if (beats.empty()) {
    const long long gridAnchorMicros =
        beatWalk.firstGridTimeMicros.value_or(startMicros);
    long long precedingBeat = gridAnchorMicros - initialBeatIntervalMicros;
    while (precedingBeat >= startMicros) {
      precedingBeat -= initialBeatIntervalMicros;
    }
    for (int beat = countInBeats - 1; beat >= 0; --beat) {
      beats.push_back({.timeMicros =
                           precedingBeat - initialBeatIntervalMicros * beat,
                       .accent = false});
    }
  }
  while (beats.size() < static_cast<std::size_t>(countInBeats)) {
    beats.push_front({.timeMicros = beats.front().timeMicros -
                                    initialBeatIntervalMicros,
                      .accent = false});
  }

  plan.clicks.reserve(static_cast<std::size_t>(countInBeats));
  for (const auto &beat : beats) {
    plan.clicks.push_back(
        {.timeMicros = beat.timeMicros, .accent = beat.accent});
  }
  plan.startTimeMicros = plan.clicks.front().timeMicros;
  plan.leadInMicros = startMicros - plan.startTimeMicros;

  // Clicks stay on the chart timeline. The rate-scaled audio clock converts
  // their chart-time spacing to real-time spacing during playback.
  (void)playback;
  return plan;
}

} // namespace prep_metronome
