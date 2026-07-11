#include "../src/PrepMetronome.h"
#include "../src/scene/play/GamePlayTiming.h"
#include "../src/scene/play/TouchVisualizationTiming.h"

#include <cmath>
#include <iostream>

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

#define ASSERT_NEAR(expected, actual, tolerance, label)                        \
  if (std::fabs((expected) - (actual)) > (tolerance)) {                        \
    std::cerr << label << " expected " << (expected) << " actual "            \
              << (actual) << std::endl;                                       \
    return 1;                                                                 \
  }

int main() {
  bms_parser::ChartMeta meta;
  meta.Bpm = 120.0;
  meta.MostPrevalentBpm = 180.0;
  meta.GuessedBeatsPerMeasure = 4;

  auto plan = prep_metronome::buildPlan(meta, true, false, 0);
  ASSERT_TRUE(plan.enabled, "enabled plan");
  ASSERT_EQ(120.0, plan.bpm, "chart bpm used");
  ASSERT_EQ(4, plan.beatsPerMeasure, "beats");
  ASSERT_EQ(-2000000LL, plan.startTimeMicros, "start time");
  ASSERT_EQ(4U, plan.clicks.size(), "click count");
  ASSERT_EQ(-2000000LL, plan.clicks[0].timeMicros, "first click");
  ASSERT_TRUE(plan.clicks[0].accent, "first accent");
  ASSERT_EQ(-500000LL, plan.clicks[3].timeMicros, "last click");

  meta.Bpm = 90.0;
  meta.MostPrevalentBpm = 90.0;
  meta.GuessedBeatBpm = 180.0;
  meta.GuessedBeatsPerMeasure = 6;
  plan = prep_metronome::buildPlan(meta, true, false, 0);
  ASSERT_TRUE(plan.enabled, "perceived bpm plan");
  ASSERT_EQ(180.0, plan.bpm, "perceived bpm used");
  ASSERT_EQ(6, plan.beatsPerMeasure, "perceived bpm beats");
  ASSERT_EQ(333333LL, plan.beatIntervalMicros, "perceived bpm interval");
  ASSERT_EQ(-1999998LL, plan.startTimeMicros, "perceived bpm start");

  meta.Bpm = 999.0;
  meta.MostPrevalentBpm = 180.0;
  meta.GuessedBeatBpm = 0.0;
  meta.GuessedBeatsPerMeasure = 4;
  plan = prep_metronome::buildPlan(meta, true, false, 3000000);
  ASSERT_TRUE(plan.enabled, "insane bpm plan");
  ASSERT_EQ(180.0, plan.bpm, "prevalent bpm used");
  ASSERT_EQ(333333LL, plan.beatIntervalMicros, "rounded beat interval");
  ASSERT_EQ(1666668LL, plan.startTimeMicros, "positive playback anchor");

  meta.MostPrevalentBpm = 500.0;
  plan = prep_metronome::buildPlan(meta, true, false, 3000000);
  ASSERT_TRUE(plan.enabled, "insane prevalent bpm plan");
  ASSERT_EQ(500.0, plan.bpm, "insane prevalent bpm still used");
  ASSERT_EQ(120000LL, plan.beatIntervalMicros,
            "insane prevalent beat interval");
  ASSERT_EQ(2520000LL, plan.startTimeMicros, "insane prevalent start");

  bms_parser::Chart chart;
  chart.Meta.Bpm = 120.0;
  chart.Meta.MostPrevalentBpm = 180.0;
  chart.Meta.GuessedBeatsPerMeasure = 4;
  auto *measure = new bms_parser::Measure();
  auto *timeline = new bms_parser::TimeLine(1, false);
  timeline->BpmChange = true;
  timeline->Bpm = 160.0;
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);
  plan = prep_metronome::buildPlan(chart, true, false, 0);
  ASSERT_TRUE(plan.enabled, "first measure bpm plan");
  ASSERT_EQ(160.0, plan.bpm, "first measure bpm used");
  ASSERT_EQ(375000LL, plan.beatIntervalMicros, "first measure bpm interval");
  ASSERT_EQ(-1500000LL, plan.startTimeMicros, "first measure bpm start");

  bms_parser::Chart laterTimelineChart;
  laterTimelineChart.Meta.Bpm = 120.0;
  laterTimelineChart.Meta.MostPrevalentBpm = 180.0;
  laterTimelineChart.Meta.GuessedBeatsPerMeasure = 4;
  auto *laterTimelineMeasure = new bms_parser::Measure();
  auto *firstTimeline = new bms_parser::TimeLine(1, false);
  firstTimeline->Bpm = 120.0;
  laterTimelineMeasure->TimeLines.push_back(firstTimeline);
  auto *laterTimeline = new bms_parser::TimeLine(1, false);
  laterTimeline->BpmChange = true;
  laterTimeline->Bpm = 160.0;
  laterTimelineMeasure->TimeLines.push_back(laterTimeline);
  laterTimelineChart.Measures.push_back(laterTimelineMeasure);
  plan = prep_metronome::buildPlan(laterTimelineChart, true, false, 0);
  ASSERT_TRUE(plan.enabled, "later bpm change plan");
  ASSERT_EQ(120.0, plan.bpm, "later first measure bpm ignored");

  plan = prep_metronome::buildPlan(meta, false, false, 0);
  ASSERT_TRUE(!plan.enabled, "disabled setting");

  plan = prep_metronome::buildPlan(meta, true, true, 0);
  ASSERT_TRUE(!plan.enabled, "preview excluded");

  bms_parser::Chart practiceChart;
  practiceChart.Meta.Bpm = 120.0;
  practiceChart.Meta.GuessedBeatsPerMeasure = 4;
  auto *practiceMeasure = new bms_parser::Measure();
  auto *practiceStartTimeline = new bms_parser::TimeLine(1, false);
  practiceStartTimeline->Timing = 0;
  practiceStartTimeline->BpmChange = true;
  practiceStartTimeline->Bpm = 120.0;
  practiceMeasure->TimeLines.push_back(practiceStartTimeline);
  auto *practiceTempoChange = new bms_parser::TimeLine(1, false);
  practiceTempoChange->Timing = 6000000;
  practiceTempoChange->BpmChange = true;
  practiceTempoChange->Bpm = 150.0;
  practiceMeasure->TimeLines.push_back(practiceTempoChange);
  auto *practiceLaterTempoChange = new bms_parser::TimeLine(1, false);
  practiceLaterTempoChange->Timing = 12000000;
  practiceLaterTempoChange->BpmChange = true;
  practiceLaterTempoChange->Bpm = 90.0;
  practiceMeasure->TimeLines.push_back(practiceLaterTempoChange);
  practiceChart.Measures.push_back(practiceMeasure);

  const audio::PlaybackRate slowPlayback{.percent = 75};
  plan = prep_metronome::buildPracticeCountInPlan(
      practiceChart, 10000000, 4, slowPlayback);
  ASSERT_TRUE(plan.enabled, "practice count-in enabled");
  ASSERT_EQ(150.0, plan.bpm, "tempo active at marker");
  ASSERT_EQ(4U, plan.clicks.size(), "practice click count");
  ASSERT_EQ(8400000LL, plan.clicks[0].timeMicros,
            "practice first click remains in chart time");
  ASSERT_EQ(9600000LL, plan.clicks[3].timeMicros,
            "practice last click remains in chart time");
  ASSERT_EQ(533333LL,
            slowPlayback.realMicrosFromChart(plan.clicks[1].timeMicros -
                                             plan.clicks[0].timeMicros),
            "practice click real spacing follows playback rate");

  ASSERT_EQ(-2000000LL,
            gameplay_timing::visualTimeMicros(-2000000LL, 0LL),
            "negative visual time without offset");
  ASSERT_EQ(-2123000LL,
            gameplay_timing::visualTimeMicros(-2000000LL, 123000LL),
            "negative visual time with offset");
  ASSERT_NEAR(1.0,
              gameplay_timing::leadInBeatDistance(0LL, -2000000LL, 120.0),
              0.000001, "negative lead-in beat distance");
  ASSERT_NEAR(0.0,
              gameplay_timing::leadInBeatDistance(0LL, 1000LL, 120.0),
              0.000001, "positive time has no lead-in beat distance");
  ASSERT_TRUE(gameplay_timing::shouldApplyPrepMetronome(true, 0ULL, 0LL),
              "prep without practice lead-in");
  ASSERT_TRUE(!gameplay_timing::shouldApplyPrepMetronome(false, 0ULL, 0LL),
              "prep setting disabled");
  ASSERT_TRUE(!gameplay_timing::shouldApplyPrepMetronome(
                  true, 5000000ULL, 10000000LL),
              "practice lead-in skips prep");
  ASSERT_TRUE(!gameplay_timing::shouldApplyPrepMetronome(
                  true, 5000000ULL, 5000000LL),
              "exact practice lead-in skips prep");
  ASSERT_TRUE(gameplay_timing::shouldApplyPrepMetronome(
                  true, 5000000ULL, 2000000LL),
              "clamped practice lead-in keeps prep");
  ASSERT_TRUE(gameplay_timing::shouldApplyPrepMetronome(
                  true, 5000000ULL, 0LL),
              "chart start practice lead-in keeps prep");
  ASSERT_TRUE(touch_visualization_timing::shouldPruneReleasedTouch(
                  true, -200000LL, 0LL, 180000LL),
              "negative release time prunes after linger");
  ASSERT_EQ(200000LL,
            touch_visualization_timing::releaseElapsedMicros(
                true, -200000LL, 0LL),
            "negative release elapsed");

  return 0;
}
