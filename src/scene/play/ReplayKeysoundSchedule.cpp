#include "ReplayKeysoundSchedule.h"

#include "GamePlayTiming.h"
#include "ManualKeysoundSelection.h"

#include <algorithm>
#include <limits>

std::vector<ReplayKeysoundEvent> resolveReplayKeysounds(
    const gameplay::GameplayDefinition &definition,
    std::span<const ReplayEvent> events,
    std::optional<gameplay::GameplayTimeRange> allowedRange) {
  std::vector<ReplayKeysoundEvent> result;
  result.reserve(events.size());
  for (const auto &event : events) {
    if (event.action != ReplayEventAction::Press ||
        (allowedRange.has_value() &&
         !allowedRange->contains(event.songTimeMicros))) {
      continue;
    }

    gameplay::NoteId soundNoteId = gameplay::kInvalidNoteId;
    if (event.noteTimeMicros >= 0) {
      if (allowedRange.has_value() &&
          !allowedRange->contains(event.noteTimeMicros)) {
        continue;
      }
      const auto laneNotes = definition.laneNotes(event.lane);
      const auto found = std::ranges::lower_bound(
          laneNotes, event.noteTimeMicros, {}, [&](gameplay::NoteId id) {
            return definition.note(id).timingMicros;
          });
      if (found != laneNotes.end() &&
          definition.note(*found).timingMicros == event.noteTimeMicros) {
        soundNoteId = *found;
      }
    } else {
      const auto laneKeysounds = definition.laneKeysoundNotes(event.lane);
      const auto selection = gameplay::selectManualKeysound(
          laneKeysounds, std::span<const gameplay::NoteId>{},
          event.songTimeMicros,
          allowedRange.has_value()
              ? allowedRange->startMicros
              : std::numeric_limits<std::int64_t>::min(),
          allowedRange.has_value()
              ? allowedRange->endMicros
              : std::numeric_limits<std::int64_t>::max(),
          [&](gameplay::NoteId id) {
            return definition.keysoundSource(id).timingMicros;
          });
      if (selection.lane == gameplay::ManualKeysoundLane::Main) {
        soundNoteId = laneKeysounds[selection.index];
      }
    }

    if (soundNoteId == gameplay::kInvalidNoteId) {
      continue;
    }
    const auto &source = definition.keysoundSource(soundNoteId);
    if (source.wav == bms_parser::Parser::NoWav) {
      continue;
    }
    result.push_back(
        {.songTimeMicros = event.songTimeMicros, .wav = source.wav});
  }
  return result;
}

std::vector<ScheduledAudioEvent> buildReplayKeysoundSchedule(
    const gameplay::GameplayDefinition &definition,
    std::span<const ReplayEvent> events, long long audioOffsetMicros,
    std::optional<gameplay::GameplayTimeRange> allowedRange) {
  std::vector<ScheduledAudioEvent> result;
  const auto keysounds =
      resolveReplayKeysounds(definition, events, allowedRange);
  result.reserve(keysounds.size());
  for (const auto &keysound : keysounds) {
    result.push_back(makeScheduledAudioEvent(
        gameplay_timing::rawSongTimeFromGameplayTime(keysound.songTimeMicros,
                                                     audioOffsetMicros),
        keysound.wav, JukeboxAudioSource::ReplayKeysound));
  }
  return result;
}
