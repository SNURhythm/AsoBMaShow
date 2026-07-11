#include "PrepMetronome.h"

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
constexpr long long kBarlineToleranceMicros = 32;

bool isMeasureStart(const bms_parser::Chart &chart, long long timeMicros) {
  return std::ranges::any_of(chart.Measures, [timeMicros](const auto *measure) {
    return measure != nullptr &&
           std::abs(measure->Timing - timeMicros) <=
               kBarlineToleranceMicros;
  });
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
  plan.leadInMicros =
      plan.beatIntervalMicros * static_cast<long long>(countInBeats);
  plan.startTimeMicros = startMicros - plan.leadInMicros;
  plan.clicks.reserve(static_cast<std::size_t>(countInBeats));
  for (int beat = 0; beat < countInBeats; ++beat) {
    plan.clicks.push_back(
        {.timeMicros = plan.startTimeMicros +
                       plan.beatIntervalMicros * static_cast<long long>(beat),
         .accent = isMeasureStart(
             chart, plan.startTimeMicros +
                        plan.beatIntervalMicros * static_cast<long long>(beat))});
  }

  // Clicks stay on the chart timeline. The rate-scaled audio clock converts
  // their chart-time spacing to real-time spacing during playback.
  (void)playback;
  return plan;
}

} // namespace prep_metronome
