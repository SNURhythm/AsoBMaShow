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
  const auto compensatedTravel = [](audio::PlaybackRate rate,
                                    long long realMicros, double bpm) {
    constexpr double referenceBpm = 120.0;
    constexpr double visibleTimeMs = 400.0 * 1000.0 / 600.0;
    const double hispeed = 240000.0 / referenceBpm / visibleTimeMs;
    const long long chartMicros = rate.chartMicrosFromReal(realMicros);
    return gameplay_timing::leadInBeatDistance(0, -chartMicros, bpm) *
           gameplay_timing::playbackTravelScale(rate) * hispeed;
  };
  const audio::PlaybackRate halfRate{.percent = 50};
  const audio::PlaybackRate normalRate{.percent = 100};
  const audio::PlaybackRate doubleRate{.percent = 200};
  const double normalLeadInTravel =
      compensatedTravel(normalRate, 800000, 120.0);
  ASSERT_NEAR(normalLeadInTravel, compensatedTravel(halfRate, 800000, 120.0),
              0.000001, "50 percent lead-in travel stays in real time");
  ASSERT_NEAR(normalLeadInTravel, compensatedTravel(doubleRate, 800000, 120.0),
              0.000001, "200 percent lead-in travel stays in real time");

  const auto bpmChangeTravel = [&](audio::PlaybackRate rate) {
    return compensatedTravel(rate, 300000, 120.0) +
           compensatedTravel(rate, 200000, 240.0);
  };
  const double normalBpmChangeTravel = bpmChangeTravel(normalRate);
  ASSERT_NEAR(normalBpmChangeTravel, bpmChangeTravel(halfRate), 0.000001,
              "50 percent BPM-change travel stays in real time");
  ASSERT_NEAR(normalBpmChangeTravel, bpmChangeTravel(doubleRate), 0.000001,
              "200 percent BPM-change travel stays in real time");

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
  practiceChart.Meta.GuessedBeatBpm = 180.0;
  practiceChart.Meta.GuessedBeatsPerMeasure = 4;
  const long long practiceMeasureStarts[] = {
      0, 2000000, 4000000, 6000000, 7600000, 9200000, 10800000, 12400000};
  for (const auto measureStart : practiceMeasureStarts) {
    auto *practiceMeasure = new bms_parser::Measure();
    practiceMeasure->Timing = measureStart;
    practiceMeasure->Scale = 1.0;
    practiceChart.Measures.push_back(practiceMeasure);
  }
  auto *practiceTempoChange = new bms_parser::TimeLine(1, false);
  practiceTempoChange->Timing = 6000000;
  practiceTempoChange->BeatPosition = 3.0;
  practiceTempoChange->BpmChange = true;
  practiceTempoChange->Bpm = 150.0;
  practiceChart.Measures[3]->TimeLines.push_back(practiceTempoChange);
  auto *practiceLaterTempoChange = new bms_parser::TimeLine(1, false);
  practiceLaterTempoChange->Timing = 12400000;
  practiceLaterTempoChange->BeatPosition = 7.0;
  practiceLaterTempoChange->BpmChange = true;
  practiceLaterTempoChange->Bpm = 90.0;
  practiceChart.Measures[7]->TimeLines.push_back(practiceLaterTempoChange);

  const audio::PlaybackRate slowPlayback{.percent = 75};
  plan = prep_metronome::buildPracticeCountInPlan(
      practiceChart, 5000000, 4, slowPlayback);
  ASSERT_EQ(120.0, plan.bpm,
            "actual chart bpm seeds practice marker tempo");

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

  bms_parser::Chart phaseChart;
  phaseChart.Meta.Bpm = 120.0;
  phaseChart.Meta.GuessedBeatsPerMeasure = 4;
  for (long long barline = 0; barline <= 12000000; barline += 2000000) {
    auto *barMeasure = new bms_parser::Measure();
    barMeasure->Timing = barline;
    phaseChart.Measures.push_back(barMeasure);
  }

  plan = prep_metronome::buildPracticeCountInPlan(
      phaseChart, 11250000, 4, slowPlayback);
  ASSERT_EQ(4U, plan.clicks.size(), "mid-beat practice click count");
  ASSERT_EQ(9500000LL, plan.clicks[0].timeMicros,
            "mid-beat marker starts on the preceding real beat grid");
  ASSERT_EQ(11000000LL, plan.clicks[3].timeMicros,
            "mid-beat marker keeps its partial final-beat gap");
  ASSERT_TRUE(!plan.clicks[0].accent,
              "mid-beat first click is not accented away from a barline");
  ASSERT_TRUE(plan.clicks[1].accent, "real barline in count-in is accented");

  plan = prep_metronome::buildPracticeCountInPlan(
      phaseChart, 11000000, 4, slowPlayback);
  ASSERT_EQ(9000000LL, plan.clicks[0].timeMicros,
            "exact-beat marker uses four strictly preceding beats");
  ASSERT_EQ(10500000LL, plan.clicks[3].timeMicros,
            "exact-beat marker itself is not emitted as a click");
  ASSERT_TRUE(plan.clicks[2].accent,
              "exact-beat count-in preserves barline phase");

  plan = prep_metronome::buildPracticeCountInPlan(
      phaseChart, 12250000, 8, slowPlayback);
  ASSERT_EQ(8500000LL, plan.clicks[0].timeMicros,
            "barline phase is inherited from the chart grid");
  ASSERT_TRUE(!plan.clicks[0].accent,
              "first click is not accented unless it is a real barline");
  ASSERT_TRUE(plan.clicks[3].accent,
              "first real barline in the span is accented");
  ASSERT_TRUE(plan.clicks[7].accent,
              "last real barline in the span is accented");

  bms_parser::Chart tripleMeterChart;
  tripleMeterChart.Meta.Bpm = 120.0;
  tripleMeterChart.Meta.GuessedBeatsPerMeasure = 3;
  for (long long barline = 0; barline <= 4500000; barline += 1500000) {
    auto *tripleMeasure = new bms_parser::Measure();
    tripleMeasure->Timing = barline;
    tripleMeasure->Scale = 0.75;
    tripleMeterChart.Measures.push_back(tripleMeasure);
  }
  plan = prep_metronome::buildPracticeCountInPlan(
      tripleMeterChart, 3250000, 4, slowPlayback);
  ASSERT_EQ(1500000LL, plan.clicks[0].timeMicros,
            "three-beat measure count-in derives from measure scale");
  ASSERT_TRUE(plan.clicks[0].accent,
              "three-beat measure starts remain accented");
  ASSERT_EQ(3000000LL, plan.clicks[3].timeMicros,
            "next three-beat barline lands after exactly three clicks");
  ASSERT_TRUE(plan.clicks[3].accent,
              "next three-beat measure start is accented");

  bms_parser::Chart tempoGridChart;
  tempoGridChart.Meta.Bpm = 120.0;
  tempoGridChart.Meta.GuessedBeatsPerMeasure = 4;
  auto *tempoMeasure = new bms_parser::Measure();
  tempoMeasure->Timing = 0;
  tempoMeasure->Scale = 1.0;
  auto *tempoStart = new bms_parser::TimeLine(1, false);
  tempoStart->Timing = 0;
  tempoStart->BeatPosition = 0.0;
  tempoStart->Bpm = 120.0;
  tempoMeasure->TimeLines.push_back(tempoStart);
  auto *tempoGridChange = new bms_parser::TimeLine(1, false);
  tempoGridChange->Timing = 1000000;
  tempoGridChange->BeatPosition = 0.5;
  tempoGridChange->BpmChange = true;
  tempoGridChange->Bpm = 60.0;
  tempoMeasure->TimeLines.push_back(tempoGridChange);
  tempoGridChart.Measures.push_back(tempoMeasure);
  auto *tempoSecondMeasure = new bms_parser::Measure();
  tempoSecondMeasure->Timing = 3000000;
  tempoSecondMeasure->Scale = 1.0;
  tempoGridChart.Measures.push_back(tempoSecondMeasure);

  plan = prep_metronome::buildPracticeCountInPlan(
      tempoGridChart, 3500000, 4, slowPlayback);
  ASSERT_EQ(60.0, plan.bpm, "marker-active BPM is reported");
  ASSERT_EQ(500000LL, plan.clicks[0].timeMicros,
            "count-in walks the real beat grid across a BPM change");
  ASSERT_EQ(3000000LL, plan.clicks[3].timeMicros,
            "count-in reaches the next parsed measure start after BPM change");
  ASSERT_TRUE(plan.clicks[3].accent,
              "parsed measure start after BPM change is accented");

  bms_parser::Chart earlyTempoChangeChart;
  earlyTempoChangeChart.Meta.Bpm = 120.0;
  earlyTempoChangeChart.Meta.GuessedBeatsPerMeasure = 4;
  auto *earlyTempoMeasure = new bms_parser::Measure();
  earlyTempoMeasure->Timing = 0;
  earlyTempoMeasure->Scale = 1.0;
  auto *earlyTempoChange = new bms_parser::TimeLine(1, false);
  earlyTempoChange->Timing = 500000;
  earlyTempoChange->BeatPosition = 0.25;
  earlyTempoChange->BpmChange = true;
  earlyTempoChange->Bpm = 240.0;
  earlyTempoMeasure->TimeLines.push_back(earlyTempoChange);
  earlyTempoChangeChart.Measures.push_back(earlyTempoMeasure);

  plan = prep_metronome::buildPracticeCountInPlan(
      earlyTempoChangeChart, 700000, 4, slowPlayback);
  ASSERT_EQ(240.0, plan.bpm,
            "early change remains the BPM active at the marker");
  ASSERT_EQ(-1000000LL, plan.clicks[0].timeMicros,
            "pre-chart count-in continues the initial tempo grid");
  ASSERT_EQ(-500000LL, plan.clicks[1].timeMicros,
            "pre-chart spacing uses the timing segment before chart zero");
  ASSERT_EQ(0LL, plan.clicks[2].timeMicros,
            "pre-chart extrapolation joins the first parsed beat");
  ASSERT_EQ(500000LL, plan.clicks[3].timeMicros,
            "early BPM change beat remains strictly before the marker");

  bms_parser::Chart stopGridChart;
  stopGridChart.Meta.Bpm = 120.0;
  stopGridChart.Meta.GuessedBeatsPerMeasure = 4;
  auto *stopMeasure = new bms_parser::Measure();
  stopMeasure->Timing = 0;
  stopMeasure->Scale = 1.0;
  auto *stopTimeline = new bms_parser::TimeLine(1, false);
  stopTimeline->Timing = 500000;
  stopTimeline->BeatPosition = 0.25;
  stopTimeline->Bpm = 120.0;
  stopTimeline->StopLength = 48.0;
  stopMeasure->TimeLines.push_back(stopTimeline);
  stopGridChart.Measures.push_back(stopMeasure);

  plan = prep_metronome::buildPracticeCountInPlan(
      stopGridChart, 2250000, 4, slowPlayback);
  ASSERT_EQ(0LL, plan.clicks[0].timeMicros,
            "stop count-in retains the measure-start beat");
  ASSERT_EQ(500000LL, plan.clicks[1].timeMicros,
            "stop begins after its grid click");
  ASSERT_EQ(1500000LL, plan.clicks[2].timeMicros,
            "next grid click includes the parsed stop duration");
  ASSERT_EQ(2000000LL, plan.clicks[3].timeMicros,
            "following grid click resumes the active BPM spacing");

  practiceTempoChange->Bpm = 480.0;
  plan = prep_metronome::buildPracticeCountInPlan(
      practiceChart, 10000000, 4, slowPlayback);
  ASSERT_EQ(480.0, plan.bpm, "marker bpm above heuristic sanity range");
  ASSERT_EQ(125000LL, plan.beatIntervalMicros,
            "fast marker beat interval remains unbounded by prep heuristics");

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
