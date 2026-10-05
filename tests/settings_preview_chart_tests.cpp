#include "scene/SettingsPreviewChart.h"
#include "bms_parser.hpp"
#include "GameplayKeyMode.h"

#include "support/AllocationLifetimeProbe.h"

#include <array>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <set>

void testRecipe() {
  const auto chart = settings_scene::makePreviewChart();
  assert(chart->Meta.Title == "Settings Preview");
  assert(chart->Meta.Bpm == 120 && chart->Meta.MinBpm == 120 && chart->Meta.MaxBpm == 120);
  assert(chart->Meta.KeyMode == 7 && !chart->Meta.IsDP && chart->Meta.Rank == 3);
  assert(chart->Meta.TotalNotes >= 52 && chart->Meta.TotalLongNotes + chart->Meta.TotalBackSpinNotes >= 4 &&
         chart->Meta.TotalScratchNotes >= 2);
  assert(chart->Meta.PlayLength == 32'500'000 && chart->Meta.TotalLength == 33'000'000);
  assert(chart->Measures.size() == 1);
  const auto &measure = *chart->Measures.front();
  assert(measure.Timing == 0 && measure.Scale == 16 && measure.Pos == 0);
  const auto &origin = *measure.TimeLines.front();
  assert(origin.Timing == 0 && origin.BeatPosition == 0 && origin.IsFirstInMeasure);
  for (const auto *note : origin.Notes) assert(note == nullptr);
  constexpr std::array<long long, 14> timings{
      500'000, 850'000, 1'200'000, 1'550'000, 1'900'000, 2'400'000, 3'900'000,
      4'300'000, 4'700'000, 5'200'000, 5'650'000, 6'100'000, 6'550'000, 7'000'000};
  constexpr std::array<int, 14> lanes{0, 2, 4, 6, 7, 3, 3, 1, 5, 0, 7, 2, 4, 6};
  assert(measure.TimeLines.size() > timings.size());
  for (std::size_t i = 0; i < timings.size(); ++i) {
    const auto &timeline = *measure.TimeLines[i + 1];
    assert(timeline.Timing == timings[i] + 1'000'000 && timeline.Bpm == 120 && timeline.Scroll == 1);
    assert(timeline.BeatPosition == static_cast<double>(timeline.Timing) / 2'000'000.0);
    assert(!timeline.IsFirstInMeasure);
    assert(timeline.Notes.size() == 16);
    for (int lane = 0; lane < 16; ++lane) {
      const auto *note = timeline.Notes[lane];
      assert((note != nullptr) == (lane == lanes[i]));
      if (note) assert(note->Lane == lane && note->Timeline == &timeline);
    }
  }
  const auto *head = dynamic_cast<bms_parser::LongNote *>(measure.TimeLines[6]->Notes[3]);
  const auto *tail = dynamic_cast<bms_parser::LongNote *>(measure.TimeLines[7]->Notes[3]);
  assert(head && tail && head->Tail == tail && tail->Head == head);
}

void testExtendedPatternCoversDifferentNoteShapes() {
  for (const int mode : settings_scene::kPreviewKeyModes) {
    const auto chart = settings_scene::makePreviewChart(mode);
    assert(chart->Meta.TotalLength == 33'000'000);
    assert(chart->Measures.front()->TimeLines.front()->Timing == 0);
    std::array<int, 4> notesPerSection{};
    int chords = 0, longHeads = 0;
    long long previousTime = -1, lastNoteTime = 0;
    for (const auto *timeline : chart->Measures.front()->TimeLines) {
      assert(timeline->Timing > previousTime);
      previousTime = timeline->Timing;
      int simultaneous = 0;
      for (const auto *note : timeline->Notes) {
        if (!note) continue;
        assert(timeline->Timing >= 1'500'000);
        lastNoteTime = timeline->Timing;
        const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
        if (longNote && longNote->IsTail()) continue;
        ++notesPerSection.at((timeline->Timing - 1'000'000) / 8'000'000);
        ++simultaneous;
        if (longNote) {
          ++longHeads;
          assert(longNote->Tail && longNote->Tail->Timeline->Timing > timeline->Timing);
        }
      }
      if (simultaneous >= 2) ++chords;
    }
    for (const int notes : notesPerSection) assert(notes >= 10);
    assert(chords >= 4 && longHeads >= 4);
    assert(lastNoteTime > 30'000'000 && chart->Meta.PlayLength == lastNoteTime);
  }
}

void testEveryConstructionAllocation() {
  const auto allocations = test_support::checkAllocationFailures([] {
    const auto chart = settings_scene::makePreviewChart();
  });
  std::cout << "Preview construction passed " << allocations << " allocation failures\n";
}

void testKeyModes() {
  for (const int mode : settings_scene::kPreviewKeyModes) {
    const auto chart = settings_scene::makePreviewChart(mode);
    assert(gameplay::presentationKeyMode(*chart) == mode);
    assert(chart->Meta.IsDP == (mode == 10 || mode == 14));
    const auto expected = mode < 0 ? chart->Meta.GetKeyLaneIndices()
                                   : chart->Meta.GetTotalLaneIndices();
    std::set<int> seen;
    int countedNotes = 0;
    int countedMines = 0;
    for (const auto *timeline : chart->Measures.front()->TimeLines) {
      for (const auto *note : timeline->Notes) {
        if (!note) continue;
        assert(note->Lane < 16 && note->Timeline == timeline);
        seen.insert(note->Lane);
        const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
        if (!longNote || !longNote->IsTail()) ++countedNotes;
      }
      for (const auto *mine : timeline->LandmineNotes) {
        if (!mine) continue;
        ++countedMines;
        assert(mine->Damage > 0 && mine->Timeline == timeline);
        assert(std::find(expected.begin(), expected.end(), mine->Lane) != expected.end());
        assert(timeline->Timing > 1'000'000 && timeline->Timing < chart->Meta.PlayLength);
        assert(timeline->Notes[mine->Lane] == nullptr);
      }
    }
    assert(seen == std::set<int>(expected.begin(), expected.end()));
    assert(countedNotes == chart->Meta.TotalNotes);
    assert(countedMines > 0 && countedMines <= 4 && chart->Meta.TotalLandmineNotes == countedMines);
  }
}

int main() {
  testExtendedPatternCoversDifferentNoteShapes();
  testRecipe();
  testEveryConstructionAllocation();
  testKeyModes();
  testRecipe();
}
