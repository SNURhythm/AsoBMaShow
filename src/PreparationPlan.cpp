#include "PreparationPlan.h"

#include <algorithm>
#include <limits>

namespace preparation {
namespace {

bool isPlayableHead(bms_parser::Note *note,
                    const bms_parser::TimeLine *timeline, int lane) {
  if (note == nullptr || note->Timeline != timeline || note->Lane != lane ||
      note->IsLandmineNote()) {
    return false;
  }
  if (!note->IsLongNote()) {
    return true;
  }
  return !static_cast<bms_parser::LongNote *>(note)->IsTail();
}

} // namespace

std::vector<int>
firstPlayableLanes(const bms_parser::Chart &chart, long long startTimeMicros,
                   std::optional<long long> endTimeMicros) {
  long long firstTiming = std::numeric_limits<long long>::max();
  std::vector<int> lanes;

  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr || timeline->Timing < startTimeMicros ||
          (endTimeMicros.has_value() &&
           timeline->Timing >= endTimeMicros.value()) ||
          timeline->Timing > firstTiming) {
        continue;
      }

      std::vector<int> timelineLanes;
      for (int lane = 0; lane < static_cast<int>(timeline->Notes.size());
           ++lane) {
        if (isPlayableHead(timeline->Notes[lane], timeline, lane)) {
          timelineLanes.push_back(lane);
        }
      }
      if (timelineLanes.empty()) {
        continue;
      }

      if (timeline->Timing < firstTiming) {
        firstTiming = timeline->Timing;
        lanes = std::move(timelineLanes);
      } else {
        lanes.insert(lanes.end(), timelineLanes.begin(), timelineLanes.end());
      }
    }
  }

  std::ranges::sort(lanes);
  lanes.erase(std::unique(lanes.begin(), lanes.end()), lanes.end());
  return lanes;
}

Plan buildNormalPlan(const bms_parser::Chart &chart, bool indicatorEnabled,
                     bool metronomeEnabled, long long playbackAnchorMicros,
                     long long noteRangeStartMicros,
                     std::optional<long long> noteRangeEndMicros,
                     audio::PlaybackRate playback) {
  Plan result;
  result.playback = playback;
  result.metronome = prep_metronome::buildPlan(
      chart, metronomeEnabled, false, playbackAnchorMicros);

  const long long indicatorEnd = result.metronome.enabled
                                     ? result.metronome.startTimeMicros
                                     : playbackAnchorMicros;
  if (indicatorEnabled) {
    result.laneIndicator.lanes = firstPlayableLanes(
        chart, noteRangeStartMicros, noteRangeEndMicros);
    if (!result.laneIndicator.lanes.empty()) {
      result.laneIndicator.endTimeMicros = indicatorEnd;
      result.laneIndicator.startTimeMicros =
          indicatorEnd -
          playback.chartMicrosFromReal(kStartLaneIndicatorRealMicros);
    }
  }

  result.playbackStartTimeMicros = result.laneIndicator.enabled()
                                       ? result.laneIndicator.startTimeMicros
                                       : indicatorEnd;
  return result;
}

Plan buildPracticePlan(const bms_parser::Chart &chart, bool indicatorEnabled,
                       long long practiceStartMicros,
                       long long practiceEndMicros, int countInBeats,
                       audio::PlaybackRate playback) {
  Plan result;
  result.playback = playback;
  result.metronome = prep_metronome::buildPracticeCountInPlan(
      chart, practiceStartMicros, countInBeats, playback);

  if (indicatorEnabled && result.metronome.enabled) {
    result.laneIndicator.lanes = firstPlayableLanes(
        chart, practiceStartMicros, practiceEndMicros);
    if (!result.laneIndicator.lanes.empty()) {
      result.laneIndicator.startTimeMicros = result.metronome.startTimeMicros;
      result.laneIndicator.endTimeMicros = practiceStartMicros;
    }
  }

  result.playbackStartTimeMicros = result.metronome.enabled
                                       ? result.metronome.startTimeMicros
                                       : practiceStartMicros;
  return result;
}

} // namespace preparation
