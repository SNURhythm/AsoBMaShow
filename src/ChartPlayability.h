#pragma once

#include "ChartTiming.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace chart_playability {
// Beatoraja follows getPair() even when the partner is displaced from a lane
// slot. Keep those identities. These diagnostic helpers do not define parser
// admission: retain the accepted graph and let each consumer perform its work.
inline std::optional<std::string>
longNoteError(const bms_parser::Chart &chart) {
  std::vector<const bms_parser::Note *> pending;
  std::unordered_set<const bms_parser::Note *> seen;
  const auto append = [&](const bms_parser::Note *note) {
    if (note != nullptr && seen.insert(note).second) pending.push_back(note);
  };
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) continue;
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) continue;
      for (const auto *note : timeline->Notes) append(note);
      for (const auto *note : timeline->InvisibleNotes) append(note);
    }
  }
  for (std::size_t index = 0; index < pending.size(); ++index) {
    const auto *ln = dynamic_cast<const bms_parser::LongNote *>(pending[index]);
    if (ln == nullptr) continue;
    const auto *partner = ln->IsTail() ? ln->Head : ln->Tail;
    if (partner == nullptr)
      return "Invalid long-note chart: an endpoint has no partner.";
    if (ln->Timeline == nullptr || partner->Timeline == nullptr)
      return "Invalid long-note chart: an endpoint has no timeline.";
    if (ln->IsTail() ? (partner->IsTail() || partner->Tail != ln)
                     : (!partner->IsTail() || partner->Head != ln))
      return "Invalid long-note chart: endpoint links are not reciprocal.";
    append(partner);
  }
  return std::nullopt;
}

inline bool inActiveSlot(const bms_parser::Note *note) noexcept {
  if (note == nullptr || note->Timeline == nullptr || note->Lane < 0) return false;
  const auto &notes = note->Timeline->Notes;
  return static_cast<std::size_t>(note->Lane) < notes.size() &&
         notes[note->Lane] == note;
}

// The active prefix matches Java's Lane traversal. Partners follow in stable
// discovery order for direct hold/release access, without becoming lane notes.
struct NoteIdentities {
  std::vector<bms_parser::Note *> notes;
  std::size_t activeCount = 0;
};
inline NoteIdentities noteIdentities(const bms_parser::Chart &chart) {
  NoteIdentities result;
  std::unordered_set<bms_parser::Note *> seen;
  const auto append = [&](bms_parser::Note *note) {
    if (note != nullptr && seen.insert(note).second) result.notes.push_back(note);
  };
  for (auto *measure : chart.Measures) {
    if (measure == nullptr) continue;
    for (auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) continue;
      for (auto *note : timeline->Notes) append(note);
      for (auto *note : timeline->LandmineNotes) append(note);
    }
  }
  result.activeCount = result.notes.size();
  for (std::size_t index = 0; index < result.notes.size(); ++index) {
    if (auto *ln = dynamic_cast<bms_parser::LongNote *>(result.notes[index]))
      append(ln->IsTail() ? ln->Head : ln->Tail);
  }
  return result;
}

// Only option preparation calls this: Java's pair has no independent lane.
// A displaced endpoint follows the surviving active endpoint after shuffling.
// Keep parser identities, timing, and active slot membership unchanged.
inline void alignDetachedLongNoteLanes(bms_parser::Chart &chart) {
  std::unordered_set<bms_parser::Note *> active;
  for (auto *measure : chart.Measures) {
    if (measure == nullptr) continue;
    for (auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) continue;
      for (auto *note : timeline->Notes) if (note != nullptr) active.insert(note);
      for (auto *note : timeline->InvisibleNotes) if (note != nullptr) active.insert(note);
    }
  }
  for (auto *note : active) {
    auto *ln = dynamic_cast<bms_parser::LongNote *>(note);
    if (ln == nullptr) continue;
    auto *pair = ln->IsTail() ? ln->Head : ln->Tail;
    if (pair != nullptr && !active.contains(pair)) pair->Lane = note->Lane;
  }
}

inline std::optional<std::string>
timingError(const bms_parser::Chart &chart) {
  constexpr auto maximum = std::numeric_limits<long long>::max();
  if (chart.Meta.TotalLength < 0 || chart.Meta.TotalLength == maximum ||
      chart.Meta.PlayLength < 0 || chart.Meta.PlayLength == maximum)
    return "Unsupported chart timing: duration is outside the playable range.";
  for (const auto *measure : chart.Measures) {
    if (measure == nullptr) continue;
    if (!std::isfinite(measure->Scale) || measure->Scale <= 0.0 ||
        measure->Timing < 0 || measure->Timing == maximum)
      return "Unsupported chart timing: invalid measure scale or timestamp.";
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) continue;
      if (timeline->Timing < 0 || timeline->Timing == maximum ||
          !std::isfinite(timeline->BeatPosition) ||
          !std::isfinite(timeline->Bpm) || timeline->Bpm <= 0.0 ||
          !std::isfinite(timeline->Scroll) || !std::isfinite(timeline->Speed))
        return "Unsupported chart timing: invalid timeline or tempo.";
      const double duration = timeline->GetStopDuration();
      if (!std::isfinite(timeline->StopLength) || timeline->StopLength < 0.0 ||
          !std::isfinite(duration) || duration < 0.0 ||
          (timeline->ParsedStopDuration && *timeline->ParsedStopDuration < 0))
        return "Unsupported chart timing: STOP must be finite and nonnegative.";
      const auto stop = chart_timing::stopDuration(*timeline);
      if (stop == maximum || timeline->Timing >= maximum - stop)
        return "Unsupported chart timing: STOP end is outside the playable range.";
    }
  }
  return std::nullopt;
}

inline std::optional<std::string> error(const bms_parser::Chart &chart) {
  if (auto failure = longNoteError(chart)) return failure;
  return timingError(chart);
}

} // namespace chart_playability
