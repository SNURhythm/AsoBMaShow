#pragma once

#include "ReplayData.h"
#include "bms_parser.hpp"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <vector>

struct ReplayGhostEvent {
  int lane = -1;
  long long noteTimeMicros = 0;
  long long judgeTimeMicros = 0;
  double judgeScrollPosition = 0.0;
  Judgement judgement = None;
};

namespace replay_ghost {

inline std::vector<ReplayGhostEvent> buildReplayGhostEvents(
    const ReplayData &replayData,
    const std::vector<const bms_parser::TimeLine *> &timelines,
    const std::unordered_map<int, size_t> &laneToOrderIndex,
    const std::function<double(long long)> &positionAtTime) {
  std::vector<ReplayGhostEvent> events;
  if (!positionAtTime) {
    return events;
  }

  for (const auto &event : replayData.events) {
    if ((event.action != ReplayEventAction::Press &&
         event.action != ReplayEventAction::Release) ||
        event.judgement == None || event.noteTimeMicros < 0) {
      continue;
    }
    if (laneToOrderIndex.find(event.lane) == laneToOrderIndex.end()) {
      continue;
    }

    const auto timelineIt = std::lower_bound(
        timelines.begin(), timelines.end(), event.noteTimeMicros,
        [](const bms_parser::TimeLine *timeline, long long timing) {
          return timeline != nullptr && timeline->Timing < timing;
        });
    if (timelineIt == timelines.end() || *timelineIt == nullptr ||
        (*timelineIt)->Timing != event.noteTimeMicros) {
      continue;
    }

    events.push_back({
        .lane = event.lane,
        .noteTimeMicros = event.noteTimeMicros,
        .judgeTimeMicros = event.judgeTimeMicros,
        .judgeScrollPosition = positionAtTime(event.judgeTimeMicros),
        .judgement = event.judgement,
    });
  }

  std::sort(events.begin(), events.end(),
            [](const ReplayGhostEvent &a, const ReplayGhostEvent &b) {
              if (a.judgeScrollPosition != b.judgeScrollPosition) {
                return a.judgeScrollPosition < b.judgeScrollPosition;
              }
              if (a.judgeTimeMicros != b.judgeTimeMicros) {
                return a.judgeTimeMicros < b.judgeTimeMicros;
              }
              if (a.noteTimeMicros != b.noteTimeMicros) {
                return a.noteTimeMicros < b.noteTimeMicros;
              }
              return a.lane < b.lane;
            });
  return events;
}

} // namespace replay_ghost
