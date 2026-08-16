#pragma once

#include "ReplayData.h"
#include "bms_parser.hpp"

#include <algorithm>
#include <optional>

namespace chart_playback_duration {

inline constexpr long long kGameplayResultTransitionDelayMicros = 2000000LL;

// Parser timelines mark authored events, not the implicit end of the final
// measure. Rendering needs that endpoint to continue scroll geometry across a
// final measure tail that contains no further event object.
struct TerminalScrollEndpoint {
  long long timeMicros = 0;
  double beatPosition = 0.0;
};

inline std::optional<TerminalScrollEndpoint>
terminalScrollEndpointAfter(const bms_parser::Chart &chart,
                            long long lastTimelineMicros,
                            double lastTimelineBeatPosition) {
  double terminalBeatPosition = 0.0;
  for (const auto *measure : chart.Measures) {
    if (measure != nullptr) {
      terminalBeatPosition += measure->Scale;
    }
  }
  const long long terminalMicros =
      std::max({0LL, chart.Meta.TotalLength, lastTimelineMicros});
  if (terminalMicros <= lastTimelineMicros ||
      terminalBeatPosition <= lastTimelineBeatPosition) {
    return std::nullopt;
  }
  return TerminalScrollEndpoint{.timeMicros = terminalMicros,
                                .beatPosition = terminalBeatPosition};
}

inline long long ChartLastTimelineMicros(const bms_parser::Chart &chart) {
  long long endMicros = 0;
  bool foundTimeline = false;
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      foundTimeline = true;
      endMicros = std::max(endMicros, timeline->Timing);
    }
  }
  if (!foundTimeline) {
    endMicros = std::max({0LL, chart.Meta.PlayLength, chart.Meta.TotalLength});
  }
  return std::max(0LL, endMicros);
}

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

inline long long GameplayEndMicros(const bms_parser::Chart &chart,
                                   long long latePoorTimingMicros) {
  return ChartLastTimelineMicros(chart) + std::max(0LL, latePoorTimingMicros);
}

inline long long GameplayResultTransitionMicros(
    const bms_parser::Chart &chart, long long latePoorTimingMicros) {
  return GameplayEndMicros(chart, latePoorTimingMicros) +
         kGameplayResultTransitionDelayMicros;
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
