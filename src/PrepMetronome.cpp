#include "PrepMetronome.h"

#include <algorithm>
#include <cmath>
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

double bpmAtChartTime(const bms_parser::Chart &chart, long long timeMicros) {
  double bpm = std::isfinite(chart.Meta.Bpm) && chart.Meta.Bpm > 0.0
                   ? chart.Meta.Bpm
                   : effectiveBpm(chart.Meta,
                                  firstMeasureBpmCandidate(chart));
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr || timeline->Timing > timeMicros) {
        continue;
      }
      if (timeline->BpmChange && std::isfinite(timeline->Bpm) &&
          timeline->Bpm > 0.0) {
        bpm = timeline->Bpm;
      }
    }
  }
  return bpm;
}

namespace {
long long beatTimeInMeasure(const bms_parser::Chart &chart,
                            const bms_parser::Measure &measure,
                            double measureBeatPosition,
                            double localBeatPosition) {
  const double targetBeatPosition = measureBeatPosition + localBeatPosition;
  double bpm = bpmAtChartTime(chart, measure.Timing);
  long long cursorTimeMicros = measure.Timing;
  double cursorBeatPosition = measureBeatPosition;

  for (const auto *timeline : measure.TimeLines) {
    if (timeline == nullptr ||
        timeline->BeatPosition + kBeatPositionTolerance <
            cursorBeatPosition ||
        timeline->BeatPosition >
            measureBeatPosition + measure.Scale + kBeatPositionTolerance) {
      continue;
    }
    if (targetBeatPosition <=
        timeline->BeatPosition + kBeatPositionTolerance) {
      if (std::abs(targetBeatPosition - timeline->BeatPosition) <=
          kBeatPositionTolerance) {
        return timeline->Timing;
      }
      break;
    }

    cursorTimeMicros =
        timeline->Timing +
        std::max(0LL, static_cast<long long>(timeline->GetStopDuration()));
    cursorBeatPosition = timeline->BeatPosition;
    if (isPositiveBpm(timeline->Bpm)) {
      bpm = timeline->Bpm;
    }
  }

  if (!isPositiveBpm(bpm)) {
    bpm = kDefaultBpm;
  }
  const double beatDistance = targetBeatPosition - cursorBeatPosition;
  return cursorTimeMicros + static_cast<long long>(std::llround(
                                kMicrosPerBmsMeasure * beatDistance / bpm));
}

std::vector<ChartBeat> chartBeatsBefore(const bms_parser::Chart &chart,
                                        long long startMicros) {
  std::vector<ChartBeat> beats;
  double measureBeatPosition = 0.0;
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr || !std::isfinite(measure->Scale) ||
        measure->Scale <= 0.0) {
      continue;
    }

    for (double localBeatPosition = 0.0;
         localBeatPosition < measure->Scale - kBeatPositionTolerance;
         localBeatPosition += kBeatPositionStep) {
      const long long timeMicros =
          localBeatPosition <= kBeatPositionTolerance
              ? measure->Timing
              : beatTimeInMeasure(chart, *measure, measureBeatPosition,
                                  localBeatPosition);
      if (timeMicros < startMicros) {
        beats.push_back({.timeMicros = timeMicros,
                         .accent = localBeatPosition <=
                                   kBeatPositionTolerance});
      }
    }
    measureBeatPosition += measure->Scale;
  }

  std::ranges::sort(beats, {}, &ChartBeat::timeMicros);
  std::vector<ChartBeat> uniqueBeats;
  uniqueBeats.reserve(beats.size());
  for (const auto &beat : beats) {
    if (!uniqueBeats.empty() &&
        uniqueBeats.back().timeMicros == beat.timeMicros) {
      uniqueBeats.back().accent = uniqueBeats.back().accent || beat.accent;
    } else {
      uniqueBeats.push_back(beat);
    }
  }
  return uniqueBeats;
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
  plan.bpm = bpmAtChartTime(chart, startMicros);
  plan.beatsPerMeasure = effectiveBeatsPerMeasure(chart.Meta);
  plan.beatIntervalMicros = beatIntervalMicrosForBpm(plan.bpm);
  auto beats = chartBeatsBefore(chart, startMicros);
  if (beats.empty()) {
    beats.reserve(static_cast<std::size_t>(countInBeats));
    for (int beat = countInBeats; beat > 0; --beat) {
      beats.push_back(
          {.timeMicros = startMicros -
                         plan.beatIntervalMicros * static_cast<long long>(beat),
           .accent = false});
    }
  }
  while (beats.size() < static_cast<std::size_t>(countInBeats)) {
    beats.insert(beats.begin(),
                 {.timeMicros = beats.front().timeMicros -
                                plan.beatIntervalMicros,
                  .accent = false});
  }

  const auto firstBeat = beats.end() - countInBeats;
  plan.clicks.reserve(static_cast<std::size_t>(countInBeats));
  for (auto beat = firstBeat; beat != beats.end(); ++beat) {
    plan.clicks.push_back(
        {.timeMicros = beat->timeMicros, .accent = beat->accent});
  }
  plan.startTimeMicros = plan.clicks.front().timeMicros;
  plan.leadInMicros = startMicros - plan.startTimeMicros;

  // Clicks stay on the chart timeline. The rate-scaled audio clock converts
  // their chart-time spacing to real-time spacing during playback.
  (void)playback;
  return plan;
}

} // namespace prep_metronome
