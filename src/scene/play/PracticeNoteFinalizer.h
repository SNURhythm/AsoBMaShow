#pragma once

#include "../../CoursePlaySession.h"
#include "NoteTimeRange.h"

#include <vector>

inline std::vector<bms_parser::Note *> finalizePendingPracticeNotes(
    bms_parser::Chart &chart, const NoteTimeRange &range,
    long long finalizationTimeMicros, int longNoteModeOverride) {
  std::vector<bms_parser::Note *> misses;
  const auto markMissed = [finalizationTimeMicros](bms_parser::Note *note,
                                                   bool dead = true) {
    note->IsPlayed = true;
    note->IsDead = dead;
    note->PlayedTime = finalizationTimeMicros;
    if (auto *longNote = dynamic_cast<bms_parser::LongNote *>(note);
        longNote != nullptr) {
      longNote->IsHolding = false;
    }
  };

  for (auto *measure : chart.Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (auto *timeline : measure->TimeLines) {
      if (timeline == nullptr || !range.contains(timeline->Timing)) {
        continue;
      }
      for (auto *note : timeline->Notes) {
        if (note == nullptr || note->IsLandmineNote()) {
          continue;
        }
        auto *longNote = dynamic_cast<bms_parser::LongNote *>(note);
        if (longNote != nullptr && !longNote->IsTail() &&
            effectiveLongNoteIsClassic(longNote, &chart,
                                       longNoteModeOverride) &&
            longNote->IsPlayed && !longNote->IsDead &&
            longNote->Tail != nullptr && !longNote->Tail->IsPlayed &&
            !range.contains(longNote->Tail)) {
          markMissed(longNote);
          misses.push_back(longNote);
          continue;
        }
        if (note->IsPlayed) {
          continue;
        }
        if (longNote == nullptr) {
          markMissed(note);
          misses.push_back(note);
          continue;
        }

        if (effectiveLongNoteIsCharge(longNote, &chart,
                                      longNoteModeOverride)) {
          markMissed(longNote);
          misses.push_back(longNote);
          continue;
        }

        if (!longNote->IsTail()) {
          markMissed(longNote);
          if (longNote->Tail != nullptr && range.contains(longNote->Tail) &&
              !longNote->Tail->IsPlayed) {
            markMissed(longNote->Tail, false);
          }
          misses.push_back(longNote);
          continue;
        }

        if (longNote->Head != nullptr && range.contains(longNote->Head) &&
            !longNote->Head->IsPlayed) {
          continue;
        }

        markMissed(longNote);
        if (longNote->Head != nullptr && range.contains(longNote->Head)) {
          longNote->Head->IsHolding = false;
        }
        misses.push_back(longNote);
      }
    }
  }
  return misses;
}
