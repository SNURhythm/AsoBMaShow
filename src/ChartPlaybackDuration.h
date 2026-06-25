#pragma once

#include "ReplayData.h"
#include "bms_parser.hpp"

#include <algorithm>

namespace chart_playback_duration {

inline long long ChartTimelineEndMicros(const bms_parser::Chart &chart) {
  long long endMicros =
      std::max({0LL, chart.Meta.TotalLength, chart.Meta.PlayLength});
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      endMicros = std::max(endMicros, timeline->Timing);
    }
  }
  return endMicros;
}

inline long long ReplayTimelineEndMicros(const bms_parser::Chart &chart,
                                         const ReplayData &replay) {
  long long endMicros = ChartTimelineEndMicros(chart);
  for (const auto &event : replay.events) {
    endMicros = std::max(endMicros, event.songTimeMicros);
    endMicros = std::max(endMicros, event.noteTimeMicros);
  }
  for (const auto &sample : replay.touchSamples) {
    endMicros = std::max(endMicros, sample.songTimeMicros);
  }
  for (const auto &event : replay.laneCoverEvents) {
    endMicros = std::max(endMicros, event.songTimeMicros);
  }
  return std::max(0LL, endMicros);
}

} // namespace chart_playback_duration
