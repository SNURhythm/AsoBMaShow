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
  assert(chart->Meta.TotalNotes == 96 && chart->Meta.TotalLongNotes == 70 &&
         chart->Meta.TotalBackSpinNotes == 10 && chart->Meta.TotalScratchNotes == 2);
  assert(chart->Meta.TotalLandmineNotes == 16);
  assert(chart->Meta.PlayLength == 32'500'000 && chart->Meta.TotalLength == 33'000'000);
  assert(chart->Measures.size() == 1);
  const auto &measure = *chart->Measures.front();
  assert(measure.Timing == 0 && measure.Scale == 16 && measure.Pos == 0);
  const auto &origin = *measure.TimeLines.front();
  assert(origin.Timing == 0 && origin.BeatPosition == 0 && origin.IsFirstInMeasure);
  for (const auto *note : origin.Notes) assert(note == nullptr);
  for (const auto *mine : origin.LandmineNotes) assert(mine == nullptr);
}

void testAllLaneChordCycles() {
  for (const int mode : settings_scene::kPreviewKeyModes) {
    const auto chart = settings_scene::makePreviewChart(mode);
    const auto lanes = mode < 0 ? chart->Meta.GetKeyLaneIndices()
                                : chart->Meta.GetTotalLaneIndices();
    const auto &timelines = chart->Measures.front()->TimeLines;
    assert(timelines.size() == 17);
    assert(chart->Meta.PlayLength == 32'500'000 && chart->Meta.TotalLength == 33'000'000);
    assert(chart->Meta.TotalNotes == 12 * lanes.size());
    assert(chart->Meta.TotalLandmineNotes == 2 * lanes.size());
    for (int cycle = 0; cycle < 2; ++cycle) {
      constexpr std::array<long long, 8> offsets{
          1'500'000, 3'500'000, 5'500'000, 7'500'000, 9'500'000, 11'500'000, 13'500'000, 16'500'000};
      constexpr std::array types{bms_parser::LongNoteType::LongNote,
                                 bms_parser::LongNoteType::ChargeNote,
                                 bms_parser::LongNoteType::HellChargeNote};
      for (int phase = 0; phase < 8; ++phase) {
        const auto *timeline = timelines[1 + cycle * 8 + phase];
        assert(timeline->Timing == cycle * 16'000'000LL + offsets[phase]);
        assert(timeline->BeatPosition == static_cast<double>(timeline->Timing) / 2'000'000.0);
        assert(timeline->Bpm == 120 && timeline->Scroll == 1 && !timeline->IsFirstInMeasure);
        for (int lane = 0; lane < 16; ++lane) {
          const bool active = std::ranges::find(lanes, lane) != lanes.end();
          const auto *note = timeline->Notes[lane];
          const auto *mine = timeline->LandmineNotes[lane];
          assert((note != nullptr) == (active && phase != 7));
          assert((mine != nullptr) == (active && phase == 7));
          if (mine) assert(mine->Lane == lane && mine->Timeline == timeline && mine->Damage > 0);
          if (!note) continue;
          assert(note->Lane == lane && note->Timeline == timeline);
          const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
          if (phase == 0) assert(longNote == nullptr);
          else if (phase % 2 == 1) {
            assert(longNote && !longNote->IsTail() && longNote->Tail);
            assert(longNote->Type == types[(phase - 1) / 2] && longNote->Tail->Type == longNote->Type);
            assert(longNote->Tail->Head == longNote && longNote->Tail->Lane == lane);
            assert(longNote->Tail->Timeline == timelines[1 + cycle * 8 + phase + 1]);
          } else {
            assert(longNote && longNote->IsTail() && longNote->Head);
            assert(longNote->Head->Tail == longNote);
          }
        }
      }
    }
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
        if (!longNote || !longNote->IsTail() ||
            longNote->Type != bms_parser::LongNoteType::LongNote) ++countedNotes;
      }
      for (const auto *mine : timeline->LandmineNotes) {
        if (!mine) continue;
        ++countedMines;
        assert(mine->Damage > 0 && mine->Timeline == timeline);
        assert(std::find(expected.begin(), expected.end(), mine->Lane) != expected.end());
        assert(timeline->Timing > 1'000'000 && timeline->Timing <= chart->Meta.PlayLength);
        assert(timeline->Notes[mine->Lane] == nullptr);
      }
    }
    assert(seen == std::set<int>(expected.begin(), expected.end()));
    assert(countedNotes == chart->Meta.TotalNotes);
    assert(countedMines == 2 * expected.size() && chart->Meta.TotalLandmineNotes == countedMines);
  }
}

int main() {
  testAllLaneChordCycles();
  testRecipe();
  testEveryConstructionAllocation();
  testKeyModes();
  testRecipe();
}
