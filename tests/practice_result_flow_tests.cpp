#include "practice/PracticeResultFlow.h"
#include "practice/PracticeResultModel.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bms_parser::Chart makeChart() {
  bms_parser::Chart chart;
  chart.Meta.PlayLength = 1'000'000;
  chart.Meta.TotalLength = 1'000'000;
  auto *measure = new bms_parser::Measure();
  measure->Timing = 0;
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = 0;
  auto *note = new bms_parser::Note(1);
  note->Lane = 1;
  note->Timeline = timeline;
  timeline->Notes.push_back(note);
  measure->TimeLines.push_back(timeline);
  chart.Measures.push_back(measure);
  return chart;
}

JudgedPlaybackData makeAutoAnalytics() {
  JudgedPlaybackData replay;
  replay.autoPlay = true;
  replay.events.push_back({.action = ReplayEventAction::Press,
                           .lane = 1,
                           .noteTimeMicros = 0,
                           .judgement = Great,
                           .diffMicros = 8'000});
  return replay;
}

void testChartViewerPracticeAutoCapturesAnalyticsOnly() {
  const auto policy = practice::resultCapturePolicy({.autoPlay = true,
                                                     .practice = true,
                                                     .replayPlayback = false,
                                                     .coursePlayback = false});
  require(policy.captureAnalytics, "live practice Auto captures analytics");
  require(!policy.recordReplay && !policy.persistReplay && !policy.persistScore,
          "practice Auto does not record or persist a replay or score");
  require(!policy.publishPracticeGhost,
          "practice Auto never publishes a practice ghost");
}

void testAnalyticsSourceDoesNotChangeRetryOrPersistenceSources() {
  auto analytics = makeAutoAnalytics();
  JudgedPlaybackData retry;
  retry.finalScore = 73;
  JudgedPlaybackData persisted;
  persisted.finalScore = 91;

  const JudgedPlaybackData *selected =
      practice::selectResultAnalyticsSource(&analytics, &persisted, &retry);
  require(selected == &analytics && selected->autoPlay,
          "dedicated analytics source has priority at the result boundary");
  require(retry.finalScore == 73 && persisted.finalScore == 91,
          "analytics selection does not mutate retry or persistence inputs");

  auto chart = makeChart();
  const std::vector<JudgedPlaybackData> attempts = {*selected};
  practice::ResultModel model(chart, attempts, 0);
  require(model.displayedAnalysis().overall.samples == 1 &&
              model.compatibilityGroups().front().containsAuto,
          "selected Auto source crosses the pure result-model boundary");
}

void testManualNormalCaptureRetainsExistingReplayPolicy() {
  const auto policy = practice::resultCapturePolicy({.autoPlay = false,
                                                     .practice = false,
                                                     .replayPlayback = false,
                                                     .coursePlayback = false});
  require(policy.captureAnalytics && policy.recordReplay &&
              policy.persistReplay && policy.persistScore &&
              !policy.publishPracticeGhost,
          "normal manual play keeps replay persistence and separate analytics");
}

} // namespace

int main() {
  testChartViewerPracticeAutoCapturesAnalyticsOnly();
  testAnalyticsSourceDoesNotChangeRetryOrPersistenceSources();
  testManualNormalCaptureRetainsExistingReplayPolicy();
  return 0;
}
