#include "scene/SettingsPreviewChart.h"
#include "scene/OffsetCalibration.h"
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
    auto timelines = chart->Measures.front()->TimeLines;
    assert(timelines.size() == 33);
    long long nextMeasure = 0;
    for (const auto *timeline : timelines) {
      if (!timeline->IsFirstInMeasure) continue;
      assert(timeline->Timing == nextMeasure);
      nextMeasure += 2'000'000;
    }
    assert(nextMeasure == 34'000'000);
    std::erase_if(timelines, [](const auto *timeline) {
      return timeline->IsFirstInMeasure && timeline->Timing != 0;
    });
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
        for (int lane = 0; lane < static_cast<int>(timeline->Notes.size()); ++lane) {
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
        assert(note->Lane < static_cast<int>(timeline->Notes.size()) && note->Timeline == timeline);
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

void testDenseAppearanceModesHaveCompletePreviewCharts() {
  for (const int mode : {9, 24, 48}) {
    assert(std::ranges::find(settings_scene::kPreviewKeyModes, mode) !=
           settings_scene::kPreviewKeyModes.end());
    const auto chart = settings_scene::makePreviewChart(mode);
    assert(chart->Meta.KeyMode == mode && !chart->Meta.IsDP);
    assert(chart->Meta.TotalNotes == mode * 12);
    assert(chart->Meta.TotalLongNotes == mode * 10);
    assert(chart->Meta.TotalLandmineNotes == mode * 2);
    assert(chart->Meta.TotalScratchNotes == 0 && chart->Meta.TotalBackSpinNotes == 0);
    int lastLaneNotes = 0, lastLaneMines = 0;
    for (const auto *timeline : chart->Measures.front()->TimeLines) {
      assert(timeline->Notes.size() >= static_cast<std::size_t>(mode));
      assert(timeline->LandmineNotes.size() >= static_cast<std::size_t>(mode));
      if (timeline->Notes[mode - 1]) ++lastLaneNotes;
      if (timeline->LandmineNotes[mode - 1]) ++lastLaneMines;
    }
    assert(lastLaneNotes == 14 && lastLaneMines == 2);
  }
}

void testCalibration() {
  using namespace settings_scene;
  // Late taps must move gameplay earlier relative to the audio clock.
  OffsetCalibration calibration;
  assert(!calibration.tap(1'020'000)); // count-in
  for (int beat = 0; beat < 16; ++beat) {
    const auto time = 3'000'000LL + beat * 500'000;
    assert(calibration.tap(time + (beat == 7 ? 160'000 : 40'000)));
    assert(!calibration.tap(time + 45'000)); // chord / duplicate
  }
  const auto estimate = calibration.estimate();
  assert(estimate && estimate->offsetMs == -40 && estimate->spreadMs == 0);
  assert(!calibration.tap(12'040'000)); // completed sample set is stable
  calibration.reset();
  assert(!calibration.estimate());
  for (int beat = 0; beat < 16; ++beat)
    assert(calibration.tap(3'000'000LL + beat * 500'000 - 25'000));
  assert(calibration.estimate()->offsetMs == 25);
  calibration.reset();
  assert(!calibration.tap(3'249'000)); // outside acceptance window
  assert(!calibration.tap(-500'000));

  for (int mode : kPreviewKeyModes) {
    const auto chart = makeCalibrationChart(mode);
    const auto lanes = chart->Meta.GetKeyLaneIndices();
    int count = 0;
    long long previous = 0;
    for (const auto *measure : chart->Measures) {
      for (const auto *timeline : measure->TimeLines) {
        for (const auto *note : timeline->Notes) {
          if (!note) continue;
          assert(dynamic_cast<const bms_parser::LongNote *>(note) == nullptr && note->Wav == bms_parser::Parser::NoWav);
          assert(std::ranges::find(lanes, note->Lane) != lanes.end());
          assert(timeline->Timing >= 3'000'000);
          if (count) assert(timeline->Timing - previous == 500'000);
          previous = timeline->Timing;
          ++count;
        }
      }
    }
    assert(count >= 32 && chart->Meta.TotalNotes == count);
    assert(chart->Meta.TotalLength > previous);
  }
}

void testCalibrationPatternsAndTempo() {
  using namespace settings_scene;
  for (const int mode : {5, -7, 14, 48}) {
    for (const int bpm : {60, 120, 240}) {
      for (const auto pattern : {CalibrationPattern::SingleLane, CalibrationPattern::Stairs,
                                CalibrationPattern::ReverseStairs, CalibrationPattern::Denim}) {
        const auto chart = makeCalibrationChart(mode, {.pattern = pattern, .lane = 1, .bpm = bpm});
        assert(chart->Meta.Bpm == bpm);
        const auto keys = chart->Meta.GetKeyLaneIndices();
        const long long beatMicros = 60'000'000 / bpm;
        int beats = 0, notes = 0;
        for (const auto *timeline : chart->Measures.front()->TimeLines) {
          std::vector<int> actual;
          for (const auto *note : timeline->Notes) if (note) actual.push_back(note->Lane);
          if (actual.empty()) continue;
          assert(timeline->Timing == (6 + beats) * beatMicros);
          std::vector<int> expected;
          if (pattern == CalibrationPattern::SingleLane) expected = {1};
          else if (pattern == CalibrationPattern::Stairs) expected = {keys[beats % keys.size()]};
          else if (pattern == CalibrationPattern::ReverseStairs)
            expected = {keys[keys.size() - 1 - beats % keys.size()]};
          else for (std::size_t i = beats % 2; i < keys.size(); i += 2) expected.push_back(keys[i]);
          std::ranges::sort(expected);
          assert(actual == expected);
          notes += actual.size();
          ++beats;
        }
        assert(beats == kCalibrationBeats && chart->Meta.TotalNotes == notes);
        assert(chart->Meta.TotalLength == 70 * beatMicros);
      }
    }
  }
  for (const int subdivision : {4, 8, 16, 24, 32}) {
    for (const int meter : {3, 4}) {
      const auto chart = makeCalibrationChart(7, {.subdivision = subdivision, .beatsPerMeasure = meter});
      const auto timing = calibrationTiming(120, meter);
      const int notesPerBeat = subdivision / 4;
      int notes = 0;
      for (const auto *timeline : chart->Measures.front()->TimeLines) {
        for (const auto *note : timeline->Notes) {
          if (!note) continue;
          assert(timeline->Timing == timing.firstNoteMicros + notes * timing.beatMicros / notesPerBeat);
          ++notes;
        }
      }
      assert(notes == kCalibrationBeats * notesPerBeat);
      assert(chart->Meta.TotalLength == timing.loopMicros);
    }
  }
  for (const int bpm : {40, 123, 300}) {
    for (const int meter : {3, 4}) {
      OffsetCalibration samples;
      samples.configure(bpm, meter);
      const auto timing = calibrationTiming(bpm, meter);
      assert(!samples.tap(timing.firstClickMicros));
      for (int beat = 0; beat < OffsetCalibration::sampleCount; ++beat)
        assert(samples.tap(timing.firstNoteMicros + beat * timing.beatMicros + 15'000));
      assert(samples.estimate()->offsetMs == -15);
    }
  }
  const auto scratch = makeCalibrationChart(7, {.lane = 7});
  assert(scratch->Meta.TotalScratchNotes == kCalibrationBeats);
}

int main() {
  testCalibrationPatternsAndTempo();
  testCalibration();
  testDenseAppearanceModesHaveCompletePreviewCharts();
  testAllLaneChordCycles();
  testRecipe();
  testEveryConstructionAllocation();
  testKeyModes();
  testRecipe();
}
