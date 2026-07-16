#include "GameplayDefinition.h"

#include "../../CoursePlaySession.h"
#include "../../bms_parser.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace gameplay {
namespace {

LongNoteRule resolveLongNoteRule(const bms_parser::LongNote *note,
                                 const bms_parser::Chart &chart,
                                 int overrideMode) {
  if (effectiveLongNoteIsHellCharge(note, chart, overrideMode)) {
    return LongNoteRule::HellCharge;
  }
  if (effectiveLongNoteIsCharge(note, chart, overrideMode)) {
    return LongNoteRule::Charge;
  }
  return LongNoteRule::Classic;
}

NoteKind noteKind(const bms_parser::Note *note) {
  if (dynamic_cast<const bms_parser::LandmineNote *>(note) != nullptr) {
    return NoteKind::Landmine;
  }
  const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
  if (longNote == nullptr) {
    return NoteKind::Normal;
  }
  return longNote->IsTail() ? NoteKind::LongTail : NoteKind::LongHead;
}

} // namespace

std::size_t GameplayDefinition::noteCount() const noexcept {
  return notes_.size();
}

const NoteDefinition &GameplayDefinition::note(NoteId id) const {
  if (id >= notes_.size()) {
    throw std::out_of_range("gameplay note id");
  }
  return notes_[id];
}

std::span<const NoteId>
GameplayDefinition::laneNotes(int lane) const noexcept {
  const auto found = std::ranges::lower_bound(
      lanes_, lane, {}, &LaneDefinition::lane);
  return found != lanes_.end() && found->lane == lane
             ? std::span<const NoteId>(found->noteIds)
             : std::span<const NoteId>();
}

std::span<const LaneDefinition> GameplayDefinition::lanes() const noexcept {
  return lanes_;
}

GameplayDefinition buildGameplayDefinition(const bms_parser::Chart &chart,
                                           int longNoteModeOverride) {
  GameplayDefinition result;
  std::unordered_map<const bms_parser::Note *, NoteId> ids;

  const auto append = [&](const bms_parser::Note *note) {
    if (note == nullptr || ids.contains(note)) {
      return;
    }
    const NoteId id = static_cast<NoteId>(result.notes_.size());
    ids.emplace(note, id);
    const auto kind = noteKind(note);
    const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
    const auto *landmine =
        dynamic_cast<const bms_parser::LandmineNote *>(note);
    result.notes_.push_back({
        .id = id,
        .lane = note->Lane,
        .timingMicros = note->Timeline->Timing,
        .wav = note->Wav,
        .kind = kind,
        .longNoteRule = longNote == nullptr
                            ? LongNoteRule::None
                            : resolveLongNoteRule(longNote, chart,
                                                  longNoteModeOverride),
        .pairId = kInvalidNoteId,
        .scratchLane = chartLaneIsScratch(chart.Meta, note->Lane),
        .mineDamage = landmine == nullptr ? 0.0F : landmine->Damage,
    });
  };

  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (const auto *note : timeline->Notes) {
        append(note);
      }
      for (const auto *note : timeline->LandmineNotes) {
        append(note);
      }
    }
  }

  for (const auto &[source, id] : ids) {
    const auto *longNote =
        dynamic_cast<const bms_parser::LongNote *>(source);
    if (longNote == nullptr) {
      continue;
    }
    const auto *pair = longNote->IsTail() ? longNote->Head : longNote->Tail;
    const auto found = ids.find(pair);
    if (found != ids.end()) {
      result.notes_[id].pairId = found->second;
    }
  }

  for (const auto &note : result.notes_) {
    if (note.kind == NoteKind::Landmine) {
      continue;
    }
    auto lane = std::ranges::lower_bound(
        result.lanes_, note.lane, {}, &LaneDefinition::lane);
    if (lane == result.lanes_.end() || lane->lane != note.lane) {
      lane = result.lanes_.insert(lane, LaneDefinition{.lane = note.lane});
    }
    lane->noteIds.push_back(note.id);
  }
  for (auto &lane : result.lanes_) {
    std::ranges::sort(lane.noteIds, [&](NoteId left, NoteId right) {
      const auto &leftNote = result.notes_[left];
      const auto &rightNote = result.notes_[right];
      return leftNote.timingMicros == rightNote.timingMicros
                 ? left < right
                 : leftNote.timingMicros < rightNote.timingMicros;
    });
  }
  return result;
}

} // namespace gameplay
