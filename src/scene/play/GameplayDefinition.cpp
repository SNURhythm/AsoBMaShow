#include "GameplayDefinition.h"

#include "../../CoursePlaySession.h"
#include "../../bms_parser.hpp"
#include "GameplayScoreState.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <utility>

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

std::span<const NoteId>
GameplayDefinition::laneKeysoundNotes(int lane) const noexcept {
  const auto found =
      std::ranges::lower_bound(lanes_, lane, {}, &LaneDefinition::lane);
  return found != lanes_.end() && found->lane == lane
             ? std::span<const NoteId>(found->keysoundNoteIds)
             : std::span<const NoteId>();
}

std::span<const LaneDefinition> GameplayDefinition::lanes() const noexcept {
  return lanes_;
}

GameplayChartMetadata GameplayDefinition::metadata() const noexcept {
  return metadata_;
}

std::span<const NoteId>
GameplayDefinition::chronologicalNotes() const noexcept {
  return chronologicalNoteIds_;
}

std::span<const NoteId>
GameplayDefinition::hellChargeHeads() const noexcept {
  return hellChargeHeadIds_;
}

GameplayDefinition buildGameplayDefinition(const bms_parser::Chart &chart,
                                           int longNoteModeOverride) {
  GameplayDefinition result;
  result.metadata_ = {
      .totalNotes = chart.Meta.TotalNotes,
      .keyMode = chart.Meta.KeyMode,
      .gaugeTotal =
          chart.Meta.HasTotal
              ? chart.Meta.Total
              : beatorajaDefaultGaugeTotal(chart.Meta.KeyMode,
                                           chart.Meta.TotalNotes),
  };
  std::unordered_map<const bms_parser::Note *, NoteId> ids;

  const auto validLanes = chart.Meta.GetTotalLaneIndices();
  result.lanes_.reserve(validLanes.size());
  for (const int lane : validLanes) {
    result.lanes_.push_back({.lane = lane});
  }
  std::ranges::sort(result.lanes_, {}, &LaneDefinition::lane);

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
      result.metadata_.finalTimelineTimeMicros =
          std::max(result.metadata_.finalTimelineTimeMicros,
                   static_cast<std::int64_t>(timeline->Timing));
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

  result.chronologicalNoteIds_.resize(result.notes_.size());
  std::iota(result.chronologicalNoteIds_.begin(),
            result.chronologicalNoteIds_.end(), NoteId{0});
  std::ranges::sort(result.chronologicalNoteIds_,
                    [&](NoteId left, NoteId right) {
                      return std::pair{result.notes_[left].timingMicros, left} <
                             std::pair{result.notes_[right].timingMicros,
                                       right};
                    });
  if (!result.chronologicalNoteIds_.empty()) {
    result.metadata_.finalNoteTimeMicros =
        result.notes_[result.chronologicalNoteIds_.back()].timingMicros;
  }
  for (const NoteId id : result.chronologicalNoteIds_) {
    const auto &note = result.notes_[id];
    if (note.kind == NoteKind::LongHead &&
        note.longNoteRule == LongNoteRule::HellCharge) {
      result.hellChargeHeadIds_.push_back(id);
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
    if (note.kind != NoteKind::LongTail) {
      lane->keysoundNoteIds.push_back(note.id);
    }
  }
  const auto noteTimingLess = [&](NoteId left, NoteId right) {
    const auto &leftNote = result.notes_[left];
    const auto &rightNote = result.notes_[right];
    return leftNote.timingMicros == rightNote.timingMicros
               ? left < right
               : leftNote.timingMicros < rightNote.timingMicros;
  };
  for (auto &lane : result.lanes_) {
    std::ranges::sort(lane.noteIds, noteTimingLess);
    std::ranges::sort(lane.keysoundNoteIds, noteTimingLess);
  }
  return result;
}

} // namespace gameplay
