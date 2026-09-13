#include "SettingsPreviewChart.h"
#include "../bms_parser.hpp"

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

std::unique_ptr<bms_parser::Chart> makePreviewChart() {
  auto chart = std::make_unique<bms_parser::Chart>();
  chart->Meta.Title = "Settings Preview";
  chart->Meta.Bpm = kPreviewBpm;
  chart->Meta.MinBpm = kPreviewBpm;
  chart->Meta.MaxBpm = kPreviewBpm;
  chart->Meta.KeyMode = 7;
  chart->Meta.IsDP = false;
  chart->Meta.Rank = 3;
  chart->Meta.PlayLength = kPreviewLoopMicros;
  chart->Meta.TotalLength = kPreviewLoopMicros;

  auto measure = std::make_unique<bms_parser::Measure>();
  measure->Timing = 0;
  measure->Scale = 4.0;
  measure->Pos = 0.0;

  auto appendTimeline = [&measure](long long timingMicros,
                                   bool firstInMeasure = false) {
    auto timeline = makePreviewTimeline(timingMicros, firstInMeasure);
    auto *timelinePtr = timeline.get();
    measure->TimeLines.push_back(timelinePtr);
    (void)timeline.release();
    return timelinePtr;
  };

  addPreviewNote(appendTimeline(500000, true), 0);
  addPreviewNote(appendTimeline(850000), 2);
  addPreviewNote(appendTimeline(1200000), 4);
  addPreviewNote(appendTimeline(1550000), 6);
  addPreviewNote(appendTimeline(1900000), 7);

  auto *longHead = appendTimeline(2400000);
  auto *longTail = appendTimeline(3900000);
  addPreviewLongNote(longHead, longTail, 3);

  addPreviewNote(appendTimeline(4300000), 1);
  addPreviewNote(appendTimeline(4700000), 5);
  addPreviewNote(appendTimeline(5200000), 0);
  addPreviewNote(appendTimeline(5650000), 7);
  addPreviewNote(appendTimeline(6100000), 2);
  addPreviewNote(appendTimeline(6550000), 4);
  addPreviewNote(appendTimeline(7000000), 6);

  chart->Measures.push_back(measure.get());
  (void)measure.release();
  return chart;
}
} // namespace settings_scene
