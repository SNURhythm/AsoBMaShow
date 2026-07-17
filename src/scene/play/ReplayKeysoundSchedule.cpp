#include "ReplayKeysoundSchedule.h"

#include "GamePlayTiming.h"

#include <algorithm>

std::vector<ScheduledAudioEvent> buildReplayKeysoundSchedule(
    const gameplay::GameplayDefinition &definition,
    std::span<const ReplayEvent> events, long long audioOffsetMicros,
    std::optional<gameplay::GameplayTimeRange> allowedRange) {
  std::vector<ScheduledAudioEvent> result;
  result.reserve(events.size());
  for (const auto &event : events) {
    if (event.action != ReplayEventAction::Press ||
        event.noteTimeMicros < 0 ||
        (allowedRange.has_value() &&
         (!allowedRange->contains(event.songTimeMicros) ||
          !allowedRange->contains(event.noteTimeMicros)))) {
      continue;
    }

    const auto laneNotes = definition.laneNotes(event.lane);
    const auto found = std::ranges::lower_bound(
        laneNotes, event.noteTimeMicros, {}, [&](gameplay::NoteId id) {
          return definition.note(id).timingMicros;
        });
    if (found == laneNotes.end()) {
      continue;
    }
    const auto &note = definition.note(*found);
    if (note.timingMicros != event.noteTimeMicros ||
        note.wav == bms_parser::Parser::NoWav) {
      continue;
    }
    result.push_back(makeScheduledAudioEvent(
        gameplay_timing::rawSongTimeFromGameplayTime(event.songTimeMicros,
                                                     audioOffsetMicros),
        note.wav, JukeboxAudioSource::ReplayKeysound));
  }
  return result;
}
