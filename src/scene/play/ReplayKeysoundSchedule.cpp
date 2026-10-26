#include "ReplayKeysoundSchedule.h"

#include "GamePlayTiming.h"
#include "ManualKeysoundSelection.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

bool replayEventAllowedForPlayback(
    const ReplayEvent &event,
    std::optional<gameplay::GameplayTimeRange> allowedRange,
    std::optional<gameplay::GameplayTimeRange> preparationRange) noexcept {
  if (!allowedRange.has_value()) {
    return true;
  }
  const bool preparationInput =
      preparationRange.has_value() && event.noteTimeMicros < 0 &&
      event.judgement == None &&
      preparationRange->contains(event.songTimeMicros);
  return preparationInput ||
         (allowedRange->contains(event.songTimeMicros) &&
          (event.noteTimeMicros < 0 ||
           allowedRange->contains(event.noteTimeMicros)));
}

std::vector<ReplayKeysoundEvent> resolveReplayKeysounds(
    const gameplay::GameplayDefinition &definition,
    std::span<const ReplayEvent> events,
    std::optional<gameplay::GameplayTimeRange> allowedRange,
    std::optional<gameplay::GameplayTimeRange> preparationRange,
    GameplayRuleset ruleset,
    std::span<const ReplayAudioUpdate> audioUpdates) {
  std::vector<ReplayKeysoundEvent> result;
  result.reserve(events.size());
  std::unordered_map<int, gameplay::NoteId> passingHeads;
  std::unordered_map<int, bool> pressedLanes;
  std::unordered_map<int, long long> suppressedBackspinPressMicros;
  std::unordered_map<gameplay::NoteId, bool> mutedHeads;
  std::unordered_map<gameplay::NoteId, std::uint64_t> headVoices;
  std::uint64_t nextVoiceId = 1;
  std::unordered_set<gameplay::NoteId> resolvedTails;
  std::unordered_set<gameplay::NoteId> failedTails;
  std::unordered_map<int, std::vector<gameplay::NoteId>> hellChargeHeads;
  for (const auto id : definition.hellChargeHeads()) {
    const auto &head = definition.note(id);
    if (!allowedRange || allowedRange->contains(head.timingMicros))
      hellChargeHeads[head.lane].push_back(id);
  }
  const auto setGain = [&](gameplay::NoteId head, bool muted, long long time) {
    const auto voice = headVoices.find(head);
    if (voice == headVoices.end() || mutedHeads[head] == muted) return;
    mutedHeads[head] = muted;
    const int wav = definition.keysoundSource(head).wav;
    if (wav != bms_parser::Parser::NoWav)
      result.push_back({.songTimeMicros = time, .wav = wav,
                        .voiceId = voice->second,
                        .setVoiceGain = true, .gain = muted ? 0.0F : 1.0F});
  };
  const auto passingHeadAt = [&](const auto &heads, long long time) {
    const auto end = std::ranges::upper_bound(heads, time, {},
        [&](gameplay::NoteId id) { return definition.note(id).timingMicros; });
    if (end == heads.begin()) return gameplay::kInvalidNoteId;
    const auto id = *std::prev(end);
    const auto &head = definition.note(id);
    return head.pairId != gameplay::kInvalidNoteId &&
                   time < definition.note(head.pairId).timingMicros
               ? id : gameplay::kInvalidNoteId;
  };
  const auto updatePassingVoices = [&](long long time) {
    for (const auto &[lane, heads] : hellChargeHeads) {
      const auto id = passingHeadAt(heads, time);
      if (id != gameplay::kInvalidNoteId && failedTails.contains(definition.note(id).pairId))
        setGain(id, !pressedLanes[lane], time);
    }
  };
  std::size_t nextAudioUpdate = 0;
  const auto applyAudioUpdates = [&](std::size_t eventIndex) {
    while (nextAudioUpdate < audioUpdates.size() &&
           audioUpdates[nextAudioUpdate].beforeEventIndex <= eventIndex) {
      const auto &update = audioUpdates[nextAudioUpdate++];
      if (allowedRange && !allowedRange->contains(update.songTimeMicros) &&
          (!preparationRange || !preparationRange->contains(update.songTimeMicros))) continue;
      if (update.lane < 0) continue;
      const auto notes = definition.laneNotes(update.lane);
      const auto head = std::ranges::lower_bound(notes, update.headTimeMicros, {},
          [&](gameplay::NoteId id) { return definition.note(id).timingMicros; });
      if (head != notes.end() && definition.note(*head).timingMicros == update.headTimeMicros &&
          definition.note(*head).kind == gameplay::NoteKind::LongHead &&
          (!allowedRange || allowedRange->contains(update.headTimeMicros)))
        setGain(*head, update.gain == 0.0F, update.songTimeMicros);
    }
  };
  std::optional<long long> lastAudioUpdateMicros;
  for (std::size_t eventIndex = 0; eventIndex < events.size(); ++eventIndex) {
    applyAudioUpdates(eventIndex);
    const auto &event = events[eventIndex];
    const bool miss = event.action == ReplayEventAction::Miss;
    const bool mine = event.action == ReplayEventAction::Mine;
    const bool release = event.action == ReplayEventAction::Release;
    if (!replayEventAllowedForPlayback(event, allowedRange, preparationRange)) {
      continue;
    }

    if (event.audioLanePressed.has_value()) pressedLanes[event.lane] = *event.audioLanePressed;
    else if (event.action == ReplayEventAction::Press) pressedLanes[event.lane] = true;
    else if (release && !event.releaseByHolding)
      pressedLanes[event.lane] = event.releaseByBackspin;
    if (audioUpdates.empty() &&
        (!lastAudioUpdateMicros || event.songTimeMicros > *lastAudioUpdateMicros)) {
      // Any reconstructed update can enter an authored interval or observe
      // another lane's held state. Auxiliary events at the same timestamp are
      // part of that update, not another pass that can undo its failed release.
      updatePassingVoices(event.songTimeMicros);
      lastAudioUpdateMicros = event.songTimeMicros;
    }
    if (!miss && !mine && !release && event.action != ReplayEventAction::Press) continue;
    if (event.action == ReplayEventAction::Press) {
      if (const auto pending = suppressedBackspinPressMicros.find(event.lane);
          pending != suppressedBackspinPressMicros.end()) {
        const bool companion = pending->second == event.songTimeMicros &&
            event.noteTimeMicros < 0 && event.judgement == None;
        suppressedBackspinPressMicros.erase(pending);
        if (companion) continue;
      }
    }
    if (miss) {
      // A tail Miss ends an accepted hold just like a resolved Release. HCNs
      // also keep controlling an earlier preview inside their authored interval.
      const auto notes = definition.chronologicalNotes();
      auto found = std::ranges::lower_bound(notes, event.noteTimeMicros, {},
          [&](gameplay::NoteId id) { return definition.note(id).timingMicros; });
      for (; found != notes.end(); ++found) {
        const auto &note = definition.note(*found);
        if (note.timingMicros != event.noteTimeMicros) break;
        if (note.lane != event.lane ||
            (note.kind != gameplay::NoteKind::LongHead && note.kind != gameplay::NoteKind::LongTail)) continue;
        if (note.kind == gameplay::NoteKind::LongTail) resolvedTails.insert(*found);
        if (note.longNoteRule != gameplay::LongNoteRule::HellCharge) continue;
        const auto headId = note.kind == gameplay::NoteKind::LongHead ? *found : note.pairId;
        if (headId == gameplay::kInvalidNoteId) continue;
        const auto &head = definition.note(headId);
        if (head.pairId == gameplay::kInvalidNoteId || event.songTimeMicros < head.timingMicros ||
            event.songTimeMicros >= definition.note(head.pairId).timingMicros) continue;
        failedTails.insert(head.pairId);
        resolvedTails.insert(head.pairId);
        setGain(headId, !pressedLanes[event.lane], event.songTimeMicros);
      }
      continue;
    }

    // Physical edges control the passing HCN even when they judge the next
    // note early. Only empty inputs are suppressed; a judged next note keeps
    // its own sound and release resolution.
    if (release || event.action == ReplayEventAction::Press) {
      if (const auto lane = hellChargeHeads.find(event.lane); lane != hellChargeHeads.end()) {
        const auto headId = passingHeadAt(lane->second, event.songTimeMicros);
        if (headId != gameplay::kInvalidNoteId) {
          const auto &head = definition.note(headId);
          if (failedTails.contains(head.pairId))
            setGain(headId, !pressedLanes[event.lane], event.songTimeMicros);
          if (event.noteTimeMicros < 0) continue;
        }
      }
    }

    // Recovery of an accepted hold keeps its voice until release resolution.
    if (const auto held = passingHeads.find(event.lane); held != passingHeads.end()) {
      const auto &head = definition.note(held->second);
      // LR2 can process an input at the authored tail before completing the
      // held LN. Replay event order tells us when that hold actually ends.
      // Beatoraja can accept a short classic head after its tail time, then
      // expire that hold without a scored tail event. An empty physical lift
      // or backspin handoff after the tail confirms recovery ownership ended.
      const bool expiredClassicLift = ruleset == GameplayRuleset::Beatoraja &&
          head.longNoteRule == gameplay::LongNoteRule::Classic && release &&
          event.noteTimeMicros < 0 &&
          (!pressedLanes[event.lane] || event.releaseByBackspin) &&
          event.songTimeMicros >= definition.note(head.pairId).timingMicros;
      if (resolvedTails.contains(head.pairId) || expiredClassicLift) {
        passingHeads.erase(held);
      } else if (event.noteTimeMicros < 0 && event.action == ReplayEventAction::Press) {
        continue; // Recovery during the release margin keeps the existing voice.
      }
    }
    if (release && (event.judgement == None || event.noteTimeMicros < 0)) continue;

    gameplay::NoteId soundNoteId = gameplay::kInvalidNoteId;
    if ((mine || release) && event.noteTimeMicros >= 0) {
      // Releases identify the tail even for classic LN. Use timed identities
      // so detached tails resolve too; rejected/deferred input edges are silent.
      // Mines are deliberately absent from the manual keysound/lane indexes.
      const auto notes = definition.chronologicalNotes();
      auto found = std::ranges::lower_bound(
          notes, event.noteTimeMicros, {}, [&](gameplay::NoteId id) {
            return definition.note(id).timingMicros;
          });
      for (; found != notes.end(); ++found) {
        const auto &note = definition.note(*found);
        if (note.timingMicros != event.noteTimeMicros) break;
        const auto kind = release ? gameplay::NoteKind::LongTail
                                  : gameplay::NoteKind::Landmine;
        if (note.kind == kind && note.lane == event.lane) {
          soundNoteId = *found;
          break;
        }
      }
    } else if (!mine && event.noteTimeMicros >= 0) {
      const auto laneNotes = definition.laneNotes(event.lane);
      const auto found = std::ranges::lower_bound(
          laneNotes, event.noteTimeMicros, {}, [&](gameplay::NoteId id) {
            return definition.note(id).timingMicros;
          });
      if (found != laneNotes.end() &&
          definition.note(*found).timingMicros == event.noteTimeMicros) {
        soundNoteId = *found;
      }
    } else if (!mine) {
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
    // A note can be triggered more than once. Controls retain the voice that
    // was current when they were issued, even when a retrigger shares its time.
    const auto voiceId = nextVoiceId++;
    const auto *note = soundNoteId < definition.noteCount()
                           ? &definition.note(soundNoteId) : nullptr;
    if (release) {
      if (!resolvedTails.insert(soundNoteId).second) continue;
      // LR2's legacy lane adapter records the opposite-key press after its
      // backspin release, but that one companion edge never starts a sound.
      // Beatoraja and classic LN releases do not use this suppression.
      if (ruleset == GameplayRuleset::LR2 && event.releaseByBackspin &&
          note != nullptr && note->pairId != gameplay::kInvalidNoteId &&
          note->scratchLane && definition.metadata().keyMode != 24 &&
          definition.metadata().keyMode != 48 &&
          note->longNoteRule != gameplay::LongNoteRule::Classic)
        suppressedBackspinPressMicros[event.lane] = event.songTimeMicros;
      if (note != nullptr && note->pairId != gameplay::kInvalidNoteId &&
          !event.releaseByHolding &&
          (event.judgement == Bad || event.judgement == Poor)) {
        failedTails.insert(soundNoteId);
        if (!(event.releaseByBackspin && note->longNoteRule != gameplay::LongNoteRule::Classic))
          setGain(note->pairId, true, event.songTimeMicros);
      }
    } else if (!mine && note != nullptr && note->kind == gameplay::NoteKind::LongHead &&
               note->pairId != gameplay::kInvalidNoteId) {
      // A fallback or KPOOR only previews the sound; it does not acquire a hold.
      // PMS BAD heads likewise remain available for another judgement.
      if (event.noteTimeMicros >= 0 && event.judgement != None &&
          event.judgement != Kpoor &&
          !(event.judgement == Bad && definition.metadata().keyMode == 9))
        passingHeads[event.lane] = soundNoteId;
      headVoices[soundNoteId] = voiceId;
      mutedHeads[soundNoteId] = false;
    }
    const auto &source = definition.keysoundSource(soundNoteId);
    if (source.wav == bms_parser::Parser::NoWav) {
      continue;
    }
    result.push_back(
        {.songTimeMicros = event.songTimeMicros, .wav = source.wav,
         .voiceId = voiceId});
  }
  applyAudioUpdates(events.size());
  return result;
}

std::vector<ScheduledAudioEvent> buildReplayKeysoundSchedule(
    const gameplay::GameplayDefinition &definition,
    std::span<const ReplayEvent> events, long long audioOffsetMicros,
    std::optional<gameplay::GameplayTimeRange> allowedRange,
    std::optional<gameplay::GameplayTimeRange> preparationRange,
    GameplayRuleset ruleset,
    std::span<const ReplayAudioUpdate> audioUpdates) {
  std::vector<ScheduledAudioEvent> result;
  const auto keysounds =
      resolveReplayKeysounds(definition, events, allowedRange,
                             preparationRange, ruleset, audioUpdates);
  result.reserve(keysounds.size());
  for (const auto &keysound : keysounds) {
    auto scheduled = makeScheduledAudioEvent(
        gameplay_timing::rawSongTimeFromGameplayTime(keysound.songTimeMicros,
                                                     audioOffsetMicros),
        keysound.wav, JukeboxAudioSource::ReplayKeysound);
    scheduled.voiceId = keysound.voiceId;
    scheduled.setVoiceGain = keysound.setVoiceGain;
    scheduled.gain = keysound.gain;
    result.push_back(scheduled);
  }
  return result;
}
