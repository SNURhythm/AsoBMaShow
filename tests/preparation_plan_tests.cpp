#include "PreparationPlan.h"

#include <iostream>
#include <optional>
#include <vector>

#define ASSERT_EQ(expected, actual, label)                                     \
  if ((expected) != (actual)) {                                                \
    std::cerr << label << " expected " << (expected) << " actual "            \
              << (actual) << std::endl;                                       \
    return 1;                                                                 \
  }

#define ASSERT_TRUE(value, label)                                              \
  if (!(value)) {                                                              \
    std::cerr << label << " expected true" << std::endl;                      \
    return 1;                                                                 \
  }

namespace {

bms_parser::TimeLine *addTimeline(bms_parser::Chart &chart,
                                  long long timingMicros) {
  if (chart.Measures.empty()) {
    chart.Measures.push_back(new bms_parser::Measure());
  }
  auto *timeline = new bms_parser::TimeLine(16, false);
  timeline->Timing = timingMicros;
  chart.Measures.front()->TimeLines.push_back(timeline);
  return timeline;
}

void addNote(bms_parser::TimeLine *timeline, int lane) {
  timeline->SetNote(lane, new bms_parser::Note(1));
}

void setChartTempo(bms_parser::Chart &chart, double bpm = 120.0) {
  chart.Meta.Bpm = bpm;
  chart.Meta.MostPrevalentBpm = bpm;
  chart.Meta.GuessedBeatBpm = bpm;
  chart.Meta.GuessedBeatsPerMeasure = 4;
}

} // namespace

int main() {
  bms_parser::Chart chart;
  setChartTempo(chart);
  addTimeline(chart, 500000);
  auto *firstPlayable = addTimeline(chart, 1000000);
  addNote(firstPlayable, 6);
  addNote(firstPlayable, 1);
  addNote(firstPlayable, 3);
  addNote(addTimeline(chart, 1500000), 5);

  ASSERT_TRUE(preparation::firstPlayableLanes(chart, 0) ==
                  std::vector<int>({1, 3, 6}),
              "first playable chord lanes are sorted");
  ASSERT_TRUE(preparation::firstPlayableLanes(chart, 1200000) ==
                  std::vector<int>({5}),
              "range start selects a later first timeline");
  ASSERT_TRUE(preparation::firstPlayableLanes(chart, 1200000, 1500000)
                  .empty(),
              "range end is exclusive");

  bms_parser::Chart filteredChart;
  setChartTempo(filteredChart);
  auto *ignored = addTimeline(filteredChart, 1000000);
  ignored->SetInvisibleNote(1, new bms_parser::Note(1));
  ignored->SetLandmineNote(2, new bms_parser::LandmineNote(10.0F));
  auto *tail = new bms_parser::LongNote(
      1, bms_parser::LongNoteType::ChargeNote);
  auto *head = new bms_parser::LongNote(
      1, bms_parser::LongNoteType::ChargeNote);
  head->Tail = tail;
  tail->Head = head;
  ignored->SetNote(3, tail);
  auto *malformed = new bms_parser::Note(1);
  ignored->Notes[4] = malformed;
  malformed->Lane = 4;
  addNote(addTimeline(filteredChart, 2000000), 7);

  ASSERT_TRUE(preparation::firstPlayableLanes(filteredChart, 0) ==
                  std::vector<int>({7}),
              "mines invisible notes tails and malformed notes are ignored");

  bms_parser::Chart longNoteChart;
  setChartTempo(longNoteChart);
  auto *longHeadTimeline = addTimeline(longNoteChart, 3000000);
  auto *longHead = new bms_parser::LongNote(
      1, bms_parser::LongNoteType::ChargeNote);
  auto *longTail = new bms_parser::LongNote(
      1, bms_parser::LongNoteType::ChargeNote);
  longHead->Tail = longTail;
  longTail->Head = longHead;
  longHeadTimeline->SetNote(2, longHead);
  addTimeline(longNoteChart, 4000000)->SetNote(2, longTail);
  ASSERT_TRUE(preparation::firstPlayableLanes(longNoteChart, 0) ==
                  std::vector<int>({2}),
              "long note head is playable");

  const audio::PlaybackRate normalRate{.percent = 100};
  auto plan = preparation::buildNormalPlan(
      chart, true, false, 0, 0, std::nullopt, normalRate);
  ASSERT_TRUE(plan.laneIndicator.enabled(),
              "indicator works with prep metronome disabled");
  ASSERT_EQ(-2000000LL, plan.playbackStartTimeMicros,
            "indicator adds two seconds before chart start");
  ASSERT_EQ(-2000000LL, plan.laneIndicator.startTimeMicros,
            "indicator starts at playback start");
  ASSERT_EQ(0LL, plan.laneIndicator.endTimeMicros,
            "indicator ends at chart start");
  ASSERT_TRUE(plan.indicatorVisibleAt(-1),
              "indicator is visible immediately before its boundary");
  ASSERT_TRUE(!plan.indicatorVisibleAt(0),
              "indicator end boundary is exclusive");
  ASSERT_EQ(-1000000LL, plan.chartTimeAtRealTime(1000000),
            "replay preparation preserves signed midpoint timestamps");

  for (const int percent : {50, 100, 200}) {
    const audio::PlaybackRate playback{.percent = percent};
    plan = preparation::buildNormalPlan(chart, true, false, 0, 0,
                                        std::nullopt, playback);
    ASSERT_EQ(2000000LL,
              playback.realMicrosFromChart(
                  plan.laneIndicator.endTimeMicros -
                  plan.laneIndicator.startTimeMicros),
              "indicator duration stays two seconds in real time");
  }

  plan = preparation::buildNormalPlan(
      chart, true, false, 0, 0, std::nullopt,
      audio::PlaybackRate{.percent = 200});
  ASSERT_EQ(3940000LL,
            plan.realTimeAtGameplayTime(4000000LL, 120000LL),
            "export converts gameplay time through the raw audio clock");
  ASSERT_EQ(5940000LL,
            plan.realTimeAtGameplayTime(4000000LL, 120000LL) + 2000000LL,
            "export result transition adds two seconds in real time");

  plan = preparation::buildNormalPlan(chart, true, true, 0, 0,
                                      std::nullopt, normalRate);
  ASSERT_TRUE(plan.metronome.enabled, "prep metronome remains enabled");
  ASSERT_EQ(-4000000LL, plan.playbackStartTimeMicros,
            "indicator precedes the four-beat metronome");
  ASSERT_EQ(-4000000LL, plan.laneIndicator.startTimeMicros,
            "indicator starts before metronome");
  ASSERT_EQ(-2000000LL, plan.laneIndicator.endTimeMicros,
            "indicator ends when metronome starts");
  ASSERT_EQ(-2000000LL, plan.metronome.startTimeMicros,
            "metronome still ends at chart start");
  ASSERT_EQ(-2000000LL, plan.skinAnimationStartTimeMicros(),
            "skin animation begins with the first prep-metronome click, not "
            "the earlier lane-indicator cue");
  ASSERT_EQ(-4000000LL, plan.chartTimeAtRealTime(0),
            "export frame zero starts at the cue origin");
  ASSERT_EQ(-2000000LL, plan.chartTimeAtRealTime(2000000),
            "export reaches the first metronome click after the cue");
  ASSERT_EQ(0LL, plan.chartTimeAtRealTime(4000000),
            "export reaches chart zero after cue and count-in");

  plan = preparation::buildNormalPlan(chart, false, true, 0, 0,
                                      std::nullopt, normalRate);
  ASSERT_TRUE(!plan.laneIndicator.enabled(), "indicator can be disabled");
  ASSERT_EQ(-2000000LL, plan.playbackStartTimeMicros,
            "disabling indicator preserves metronome lead-in");
  ASSERT_EQ(-2000000LL, plan.skinAnimationStartTimeMicros(),
            "skin animation still begins at the prep metronome without lane "
            "indicators");

  plan = preparation::buildNormalPlan(chart, true, false, 0, 0,
                                      std::nullopt, normalRate);
  ASSERT_EQ(plan.playbackStartTimeMicros, plan.skinAnimationStartTimeMicros(),
            "without a prep metronome skin animation retains the playback "
            "origin");

  bms_parser::Chart emptyChart;
  setChartTempo(emptyChart);
  plan = preparation::buildNormalPlan(emptyChart, true, false, 0, 0,
                                      std::nullopt, normalRate);
  ASSERT_EQ(0LL, plan.playbackStartTimeMicros,
            "empty charts do not add indicator delay");

  plan = preparation::buildPracticePlan(chart, true, 1200000, 2000000, 4,
                                        normalRate);
  ASSERT_TRUE(plan.metronome.enabled, "practice count-in remains enabled");
  ASSERT_TRUE(plan.laneIndicator.enabled(),
              "practice shows the later first playable lane");
  ASSERT_TRUE(plan.laneIndicator.lanes == std::vector<int>({5}),
              "practice indicator targets the selected range");
  ASSERT_EQ(plan.metronome.startTimeMicros,
            plan.laneIndicator.startTimeMicros,
            "practice indicator starts with the existing count-in");
  ASSERT_EQ(1200000LL, plan.laneIndicator.endTimeMicros,
            "practice indicator ends at the practice marker");
  ASSERT_EQ(plan.metronome.startTimeMicros, plan.playbackStartTimeMicros,
            "practice adds no extra pre-count-in time");

  std::cout << "preparation plan tests passed" << std::endl;
  return 0;
}
