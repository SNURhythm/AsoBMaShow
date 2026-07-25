#pragma once

#include "analysis/JudgedPlaybackData.h"
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

struct ReplayMissMarker {
  int lane = -1;
  long long noteTimeMicros = 0;
  double noteScrollPosition = 0.0;
};

namespace replay_ghost {

inline bool hasTimelineAt(
    const std::vector<const bms_parser::TimeLine *> &timelines,
    long long noteTimeMicros) {
  const auto timelineIt = std::lower_bound(
      timelines.begin(), timelines.end(), noteTimeMicros,
      [](const bms_parser::TimeLine *timeline, long long timing) {
        return timeline != nullptr && timeline->Timing < timing;
      });
  return timelineIt != timelines.end() && *timelineIt != nullptr &&
         (*timelineIt)->Timing == noteTimeMicros;
}

inline std::vector<ReplayGhostEvent> buildReplayGhostEvents(
    const JudgedPlaybackData &replayData,
    const std::vector<const bms_parser::TimeLine *> &timelines,
    const std::unordered_map<int, size_t> &laneToOrderIndex,
    const std::function<double(long long)> &positionAtTime) {
  std::vector<ReplayGhostEvent> events;
  if (!positionAtTime) {
    return events;
  }

  for (const auto &event : replayData.events) {
    const bool isJudgedNoteEvent =
        event.action == ReplayEventAction::Press ||
        event.action == ReplayEventAction::MultiBad ||
        event.action == ReplayEventAction::Release;
    if (!isJudgedNoteEvent || event.judgement == None ||
        event.noteTimeMicros < 0) {
      continue;
    }
    if (laneToOrderIndex.find(event.lane) == laneToOrderIndex.end()) {
      continue;
    }
    if (!hasTimelineAt(timelines, event.noteTimeMicros)) {
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

inline std::vector<ReplayMissMarker> buildReplayMissMarkers(
    const JudgedPlaybackData &replayData,
    const std::vector<const bms_parser::TimeLine *> &timelines,
    const std::unordered_map<int, size_t> &laneToOrderIndex,
    const std::function<double(long long)> &positionAtTime) {
  std::vector<ReplayMissMarker> markers;
  if (!positionAtTime) {
    return markers;
  }

  for (const auto &event : replayData.events) {
    if (event.action != ReplayEventAction::Miss || event.noteTimeMicros < 0) {
      continue;
    }
    if (laneToOrderIndex.find(event.lane) == laneToOrderIndex.end()) {
      continue;
    }
    if (!hasTimelineAt(timelines, event.noteTimeMicros)) {
      continue;
    }

    markers.push_back({
        .lane = event.lane,
        .noteTimeMicros = event.noteTimeMicros,
        .noteScrollPosition = positionAtTime(event.noteTimeMicros),
    });
  }

  std::sort(markers.begin(), markers.end(),
            [](const ReplayMissMarker &a, const ReplayMissMarker &b) {
              if (a.noteScrollPosition != b.noteScrollPosition) {
                return a.noteScrollPosition < b.noteScrollPosition;
              }
              if (a.noteTimeMicros != b.noteTimeMicros) {
                return a.noteTimeMicros < b.noteTimeMicros;
              }
              return a.lane < b.lane;
            });
  return markers;
}

} // namespace replay_ghost
