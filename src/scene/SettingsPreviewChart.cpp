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
                        bms_parser::TimeLine *tailTimeline, int lane,
                        bms_parser::LongNoteType type) {
  auto head = std::make_unique<bms_parser::LongNote>(bms_parser::Parser::NoWav, type);
  auto tail = std::make_unique<bms_parser::LongNote>(bms_parser::Parser::NoWav, type);
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
  chart->Meta.Artist = "AsoBMaShow";
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

  const auto lanes = keyMode < 0 ? chart->Meta.GetKeyLaneIndices()
                                 : chart->Meta.GetTotalLaneIndices();
  constexpr std::array longTypes{bms_parser::LongNoteType::LongNote,
                                 bms_parser::LongNoteType::ChargeNote,
                                 bms_parser::LongNoteType::HellChargeNote};
  // Two complete all-lane sequences retain the 33-second preview and its
  // 32.5-second last event, including time to inspect all three hold types.
  for (int cycle = 0; cycle < 2; ++cycle) {
    const long long start = cycle * 16'000'000LL;
    auto *normal = appendTimeline(start + 1'500'000);
    for (const int lane : lanes) addPreviewNote(normal, lane);
    for (std::size_t index = 0; index < longTypes.size(); ++index) {
      const long long headTime = start + 3'500'000 + index * 4'000'000LL;
      auto *head = appendTimeline(headTime);
      auto *tail = appendTimeline(headTime + 2'000'000);
      for (const int lane : lanes) addPreviewLongNote(head, tail, lane, longTypes[index]);
    }
    auto *mines = appendTimeline(start + 16'500'000);
    for (const int lane : lanes) {
      auto mine = std::make_unique<bms_parser::LandmineNote>(5.0F);
      mines->SetLandmineNote(lane, mine.release());
    }
  }

  // Regular bar lines make measure-line appearance visible throughout the preview.
  for (long long time = 0; time < kPreviewLoopMicros; time += 2'000'000)
    appendTimeline(time, true);
  std::ranges::sort(measure->TimeLines, {}, &bms_parser::TimeLine::Timing);
  const auto scratches = chart->Meta.GetScratchLaneIndices();
  for (const auto *timeline : measure->TimeLines) {
    for (const auto *note : timeline->Notes) {
      if (note == nullptr) continue;
      chart->Meta.PlayLength = std::max(chart->Meta.PlayLength, timeline->Timing);
      const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
      if (longNote && longNote->IsTail() &&
          longNote->Type == bms_parser::LongNoteType::LongNote) continue;
      ++chart->Meta.TotalNotes;
      const bool scratch = std::ranges::find(scratches, note->Lane) != scratches.end();
      if (longNote) {
        if (scratch) ++chart->Meta.TotalBackSpinNotes;
        else ++chart->Meta.TotalLongNotes;
      } else if (scratch) ++chart->Meta.TotalScratchNotes;
    }
    for (const auto *mine : timeline->LandmineNotes) {
      if (!mine) continue;
      ++chart->Meta.TotalLandmineNotes;
      chart->Meta.PlayLength = std::max(chart->Meta.PlayLength, timeline->Timing);
    }
  }

  chart->Measures.push_back(measure.get());
  (void)measure.release();
  return chart;
}
} // namespace settings_scene
