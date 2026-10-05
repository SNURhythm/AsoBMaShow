#include "SettingsPreviewChart.h"
#include "../bms_parser.hpp"
#include <algorithm>
#include <stdexcept>

namespace settings_scene {
namespace {
constexpr int kPreviewTimelineLanes = 16;

std::unique_ptr<bms_parser::TimeLine>
makePreviewTimeline(long long timingMicros, bool firstInMeasure = false) {
  auto timeline =
      std::make_unique<bms_parser::TimeLine>(kPreviewTimelineLanes, false);
  timeline->Timing = timingMicros;
  timeline->BeatPosition = static_cast<double>(timingMicros) / 2000000.0;
  timeline->Bpm = kPreviewBpm;
  timeline->Scroll = 1.0;
  timeline->IsFirstInMeasure = firstInMeasure;
  return timeline;
}

void addPreviewNote(bms_parser::TimeLine *timeline, int lane) {
  auto note = std::make_unique<bms_parser::Note>(bms_parser::Parser::NoWav);
  timeline->SetNote(lane, note.release());
}

void addPreviewLongNote(bms_parser::TimeLine *headTimeline,
                        bms_parser::TimeLine *tailTimeline, int lane) {
  auto head = std::make_unique<bms_parser::LongNote>(bms_parser::Parser::NoWav);
  auto tail = std::make_unique<bms_parser::LongNote>(bms_parser::Parser::NoWav);
  head->Tail = tail.get();
  tail->Head = head.get();
  headTimeline->SetNote(lane, head.release());
  tailTimeline->SetNote(lane, tail.release());
}

} // namespace

std::unique_ptr<bms_parser::Chart> makePreviewChart(int keyMode) {
  if (std::ranges::find(kPreviewKeyModes, keyMode) == kPreviewKeyModes.end())
    throw std::invalid_argument("Unsupported preview key mode");
  auto chart = std::make_unique<bms_parser::Chart>();
  chart->Meta.Title = "Settings Preview";
  chart->Meta.SubTitle = "Sample Chart";
  chart->Meta.Artist = "SNURhythm";
  chart->Meta.SubArtist = "AsoBMaShow Preview";
  chart->Meta.Genre = "PRACTICE";
  chart->Meta.PlayLevelText = "5";
  chart->Meta.Difficulty = 2;
  chart->Meta.Bpm = kPreviewBpm;
  chart->Meta.MinBpm = kPreviewBpm;
  chart->Meta.MaxBpm = kPreviewBpm;
  chart->Meta.KeyMode = std::abs(keyMode);
  chart->Meta.IsDP = keyMode == 10 || keyMode == 14;
  chart->Meta.Rank = 3;
  chart->Meta.PlayLength = 0;
  chart->Meta.TotalLength = kPreviewLoopMicros;

  auto measure = std::make_unique<bms_parser::Measure>();
  measure->Timing = 0;
  measure->Scale = 16.0;
  measure->Pos = 0.0;

  auto appendTimeline = [&measure](long long timingMicros,
                                   bool firstInMeasure = false) {
    auto timeline = makePreviewTimeline(timingMicros, firstInMeasure);
    auto *timelinePtr = timeline.get();
    measure->TimeLines.push_back(timelinePtr);
    (void)timeline.release();
    return timelinePtr;
  };

  // Keep the original 7K1S pattern, and map other modes onto their real parser lanes.
  const auto lanes = keyMode < 0 ? chart->Meta.GetKeyLaneIndices()
                                 : chart->Meta.GetTotalLaneIndices();
  const auto laneFor = [&](int legacyLane) {
    return keyMode == 7 ? legacyLane : lanes[legacyLane % lanes.size()];
  };
  addPreviewNote(appendTimeline(500000), laneFor(0));
  addPreviewNote(appendTimeline(850000), laneFor(2));
  addPreviewNote(appendTimeline(1200000), laneFor(4));
  addPreviewNote(appendTimeline(1550000), laneFor(6));
  addPreviewNote(appendTimeline(1900000), laneFor(7));

  auto *longHead = appendTimeline(2400000);
  auto *longTail = appendTimeline(3900000);
  addPreviewLongNote(longHead, longTail, laneFor(3));

  addPreviewNote(appendTimeline(4300000), laneFor(1));
  addPreviewNote(appendTimeline(4700000), laneFor(5));
  addPreviewNote(appendTimeline(5200000), laneFor(0));
  addPreviewNote(appendTimeline(5650000), laneFor(7));
  addPreviewNote(appendTimeline(6100000), laneFor(2));
  addPreviewNote(appendTimeline(6550000), laneFor(4));
  addPreviewNote(appendTimeline(7000000), laneFor(6));
  for (std::size_t index = 8; index < lanes.size(); ++index)
    addPreviewNote(measure->TimeLines[((index - 8) * 2) % measure->TimeLines.size()], lanes[index]);

  const int laneCount = static_cast<int>(lanes.size());
  const auto noteAt = [&](long long time, int laneIndex) {
    addPreviewNote(appendTimeline(time), lanes[laneIndex % laneCount]);
  };
  const auto chordAt = [&](long long time, std::initializer_list<int> laneIndices) {
    auto *timeline = appendTimeline(time);
    for (const int index : laneIndices)
      addPreviewNote(timeline, lanes[index % laneCount]);
  };
  const auto longAt = [&](long long head, long long tail, int laneIndex) {
    addPreviewLongNote(appendTimeline(head), appendTimeline(tail),
                      lanes[laneIndex % laneCount]);
  };

  // Ascending runs, two-note chords, and a sustained note.
  for (int step = 0; step < 8; ++step)
    noteAt(8'500'000 + step * 250'000, step);
  chordAt(10'750'000, {0, laneCount / 2});
  longAt(11'500'000, 13'000'000, 3);
  chordAt(13'500'000, {1, laneCount - 1});
  chordAt(14'000'000, {0, laneCount / 2});
  chordAt(14'500'000, {1, laneCount - 1});
  noteAt(15'250'000, laneCount - 1);

  // Descending runs, repeated taps, then a pair of held notes.
  for (int step = 0; step < 8; ++step)
    noteAt(16'500'000 + step * 250'000, laneCount - 1 - step % laneCount);
  for (int step = 0; step < 4; ++step)
    noteAt(19'000'000 + step * 250'000, 0);
  auto *dualHead = appendTimeline(20'250'000);
  auto *dualTail = appendTimeline(21'750'000);
  for (const int index : {0, laneCount / 2})
    addPreviewLongNote(dualHead, dualTail, lanes[index]);
  chordAt(22'250'000, {1, laneCount - 1});
  noteAt(23'250'000, laneCount / 2);

  // Alternate sides, play around a hold, and finish with a chord.
  for (int step = 0; step < 8; ++step)
    noteAt(24'500'000 + step * 250'000,
           step % 2 == 0 ? step / 2 : laneCount - 1 - step / 2);
  chordAt(27'000'000, {0, laneCount / 2});
  longAt(27'500'000, 29'500'000, 0);
  noteAt(28'000'000, 1);
  noteAt(28'500'000, laneCount - 1);
  for (int step = 0; step < 4; ++step)
    noteAt(30'000'000 + step * 250'000, step);
  chordAt(31'500'000, {0, laneCount / 2, laneCount - 1});

  // Delay the complete pattern, including long-note tails, by one second.
  for (auto *timeline : measure->TimeLines) {
    timeline->Timing += 1'000'000;
    timeline->BeatPosition = static_cast<double>(timeline->Timing) / 2'000'000.0;
  }
  // A normal empty chart origin keeps all rows scrolling before the first note.
  appendTimeline(0, true);
  std::ranges::sort(measure->TimeLines, {}, &bms_parser::TimeLine::Timing);
  const auto scratches = chart->Meta.GetScratchLaneIndices();
  for (const auto *timeline : measure->TimeLines) {
    for (const auto *note : timeline->Notes) {
      if (note == nullptr) continue;
      chart->Meta.PlayLength = std::max(chart->Meta.PlayLength, timeline->Timing);
      const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
      if (longNote && longNote->IsTail()) continue;
      ++chart->Meta.TotalNotes;
      const bool scratch = std::ranges::find(scratches, note->Lane) != scratches.end();
      if (longNote) {
        if (scratch) ++chart->Meta.TotalBackSpinNotes;
        else ++chart->Meta.TotalLongNotes;
      } else if (scratch) ++chart->Meta.TotalScratchNotes;
    }
  }

  chart->Measures.push_back(measure.get());
  (void)measure.release();
  return chart;
}
} // namespace settings_scene
