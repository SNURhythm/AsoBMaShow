#include "ClubBeat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace club_beat {
namespace {
constexpr double kDefaultBpm = 120.0;
constexpr double kMicrosPerBmsMeasure = 240000000.0;
constexpr double kBeatPositionStep = 0.25;
constexpr double kTolerance = 0.000001;
constexpr double kPi = 3.14159265358979323846;

bool validBpm(double bpm) {
  return std::isfinite(bpm) && bpm > 0.0;
}

double initialBpm(const bms_parser::Chart &chart) {
  return validBpm(chart.Meta.Bpm) ? chart.Meta.Bpm : kDefaultBpm;
}

float clampSample(double value) {
  return static_cast<float>(std::clamp(value, -1.0, 1.0));
}
} // namespace

std::vector<Event> buildPlan(const bms_parser::Chart &chart) {
  std::vector<Event> result;
  double activeBpm = initialBpm(chart);
  double measureBeatPosition = 0.0;

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
      if (timeline.BpmChange && validBpm(timeline.Bpm)) {
        activeBpm = timeline.Bpm;
      }
    };

    int beatInMeasure = 1;
    for (double localBeatPosition = 0.0;
         localBeatPosition < measure->Scale - kTolerance;
         localBeatPosition += kBeatPositionStep, ++beatInMeasure) {
      const double targetBeatPosition =
          measureBeatPosition + localBeatPosition;
      while (timelineIndex < measure->TimeLines.size()) {
        const auto *timeline = measure->TimeLines[timelineIndex];
        if (timeline == nullptr ||
            timeline->BeatPosition + kTolerance <
                timingCursorBeatPosition) {
          ++timelineIndex;
          continue;
        }
        if (timeline->BeatPosition + kTolerance >= targetBeatPosition) {
          break;
        }
        processTimeline(*timeline);
        ++timelineIndex;
      }

      if (!validBpm(activeBpm)) {
        activeBpm = kDefaultBpm;
      }
      long long timeMicros = measure->Timing;
      if (localBeatPosition > kTolerance) {
        const double beatDistance =
            targetBeatPosition - timingCursorBeatPosition;
        timeMicros = timingCursorMicros +
                     static_cast<long long>(std::llround(
                         kMicrosPerBmsMeasure * beatDistance / activeBpm));
      }

      while (timelineIndex < measure->TimeLines.size()) {
        const auto *timeline = measure->TimeLines[timelineIndex];
        if (timeline == nullptr ||
            timeline->BeatPosition + kTolerance <
                targetBeatPosition) {
          ++timelineIndex;
          continue;
        }
        if (std::abs(timeline->BeatPosition - targetBeatPosition) >
            kTolerance) {
          break;
        }
        timeMicros = timeline->Timing;
        processTimeline(*timeline);
        ++timelineIndex;
      }

      result.push_back({.timeMicros = timeMicros,
                        .beatInMeasure = beatInMeasure,
                        .kick = true,
                        .clap = beatInMeasure == 2 || beatInMeasure == 4});
    }

    while (timelineIndex < measure->TimeLines.size()) {
      const auto *timeline = measure->TimeLines[timelineIndex++];
      if (timeline != nullptr) {
        processTimeline(*timeline);
      }
    }
    measureBeatPosition += measure->Scale;
  }
  return result;
}

StereoSound synthesizeKick(int sampleRate) {
  sampleRate = std::max(1, sampleRate);
  constexpr double durationSeconds = 0.22;
  const int frames =
      std::max(1, static_cast<int>(std::lround(sampleRate * durationSeconds)));
  StereoSound result{.sampleRate = sampleRate};
  result.samples.resize(static_cast<std::size_t>(frames) * 2);

  double phase = 0.0;
  for (int frame = 0; frame < frames; ++frame) {
    const double time = static_cast<double>(frame) / sampleRate;
    const double frequency = 48.0 + 105.0 * std::exp(-time * 28.0);
    phase += 2.0 * kPi * frequency / sampleRate;
    const double body = std::sin(phase) * std::exp(-time * 18.0) * 0.55;
    const double transient =
        (frame < sampleRate / 500 ? 1.0 - frame * 500.0 / sampleRate : 0.0) *
        0.16;
    const float sample = clampSample(body + transient);
    result.samples[static_cast<std::size_t>(frame) * 2] = sample;
    result.samples[static_cast<std::size_t>(frame) * 2 + 1] = sample;
  }
  return result;
}

StereoSound synthesizeClap(int sampleRate) {
  sampleRate = std::max(1, sampleRate);
  constexpr double durationSeconds = 0.14;
  const int frames =
      std::max(1, static_cast<int>(std::lround(sampleRate * durationSeconds)));
  StereoSound result{.sampleRate = sampleRate};
  result.samples.resize(static_cast<std::size_t>(frames) * 2);

  std::uint32_t state = 0x6d2b79f5u;
  double previousNoise = 0.0;
  for (int frame = 0; frame < frames; ++frame) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const double noise =
        static_cast<double>(state) /
            static_cast<double>(UINT32_MAX) *
            2.0 -
        1.0;
    const double highPassed = noise - previousNoise * 0.88;
    previousNoise = noise;

    const double time = static_cast<double>(frame) / sampleRate;
    double envelope = 0.0;
    for (const double burst : {0.0, 0.018, 0.036}) {
      if (time >= burst) {
        envelope += std::exp(-(time - burst) * 62.0);
      }
    }
    const float sample = clampSample(highPassed * envelope * 0.14);
    result.samples[static_cast<std::size_t>(frame) * 2] = sample;
    result.samples[static_cast<std::size_t>(frame) * 2 + 1] = sample;
  }
  return result;
}

} // namespace club_beat

