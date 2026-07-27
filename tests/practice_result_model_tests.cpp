#include "practice/PracticeResultModel.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bms_parser::TimeLine *addNote(bms_parser::Measure &measure,
                              long long timingMicros, int lane) {
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = timingMicros;
  auto *note = new bms_parser::Note(1);
  note->Lane = lane;
  note->Timeline = timeline;
  timeline->Notes.push_back(note);
  measure.TimeLines.push_back(timeline);
  return timeline;
}

bms_parser::Chart makeChart() {
  bms_parser::Chart chart;
  chart.Meta.SHA256 = std::string(64, 'a');
  chart.Meta.MD5 = std::string(32, 'b');
  chart.Meta.PlayLength = 3'000'000;
  chart.Meta.TotalLength = 3'000'000;
  for (int index = 0; index < 3; ++index) {
    auto *measure = new bms_parser::Measure();
    measure->Timing = static_cast<long long>(index) * 1'000'000;
    addNote(*measure, measure->Timing, index + 1);
    chart.Measures.push_back(measure);
  }
  return chart;
}

std::vector<analysis::JudgedWindow> windows(long long early = -10'000) {
  return {{.judgement = PGreat,
           .earlyMicros = early,
           .lateMicros = 10'000},
          {.judgement = Great,
           .earlyMicros = -30'000,
           .lateMicros = 30'000}};
}

JudgedPlaybackData makeAttempt(int lane, long long noteMicros, long long diffMicros,
                       int playbackPercent = 100, bool autoPlay = false) {
  JudgedPlaybackData replay;
  replay.autoPlay = autoPlay;
  replay.setup.playbackRatePercent = playbackPercent;
  replay.setup.judgeWindowScalePercent = 100;
  replay.context.ruleset = RulesetDescriptor::Current();
  replay.context.policy = analysis::PlaybackPolicySnapshot{
      .chartMd5 = std::string(32, 'b'),
      .chartSha256 = std::string(64, 'a'),
      .effectiveJudgeWindows = windows(),
  };
  replay.events.push_back({.action = ReplayEventAction::Press,
                           .lane = lane,
                           .noteTimeMicros = noteMicros,
                           .judgement = Great,
                           .diffMicros = diffMicros});
  return replay;
}

void testAggregatesOnlyCompatibleCompletedAttempts() {
  auto chart = makeChart();
  std::vector<JudgedPlaybackData> attempts = {
      makeAttempt(1, 0, -5'000),
      makeAttempt(2, 1'000'000, 5'000),
      makeAttempt(3, 2'000'000, 10'000, 75),
  };

  practice::ResultModel model(chart, attempts, 1);
  require(model.completedAttempts() == 3, "completed attempt count");
  require(model.abandonedAttempts() == 1, "abandoned attempt count");
  require(model.compatibilityGroups().size() == 2,
          "incompatible timing conditions remain separate");
  require(model.displayedAnalysis().overall.samples == 2,
          "default aggregate contains only its compatible group");
  require(
      model.compatibilityGroups()[0].label.find("100%") != std::string::npos &&
          model.compatibilityGroups()[1].label.find("75%") != std::string::npos,
      "group labels expose playback compatibility conditions");
  require(model.compatibilityGroups()[0].label.starts_with("Group 1") &&
              model.compatibilityGroups()[1].label.starts_with("Group 2"),
          "compatibility groups have stable visible labels");

  model.selectAggregateGroup(1);
  require(model.displayedAnalysis().overall.samples == 1,
          "alternate aggregate selects exactly one compatibility group");
}

void testAttemptSelectionAndAutoLabels() {
  auto chart = makeChart();
  std::vector<JudgedPlaybackData> attempts = {
      makeAttempt(1, 0, -5'000),
      makeAttempt(2, 1'000'000, 7'000, 100, true),
  };
  practice::ResultModel model(chart, attempts, 2);

  model.selectAttempt(1);
  require(model.selectedAttempt() == std::optional<std::size_t>(1),
          "attempt selection is retained");
  require(model.displayedAnalysis().overall.samples == 1,
          "attempt selection displays one loop");
  require(model.attemptLabel(1).find("Auto") != std::string::npos,
          "auto-play loop is explicitly labeled Auto");
  require(model.attemptLabel(1).find("Group 1") != std::string::npos,
          "individual loops expose their compatibility group");
  require(model.displayedIsAuto(), "auto attempt state is exposed to the UI");

  model.selectAttempt(99);
  require(!model.selectedAttempt().has_value(),
          "invalid attempt selection returns to aggregate");
}

void testSectionSelectionUsesExactMeasureBoundaries() {
  auto chart = makeChart();
  std::vector<JudgedPlaybackData> attempts = {makeAttempt(1, 0, -5'000)};
  practice::ResultModel model(chart, attempts, 0);

  model.selectSection(2, 1);
  require(model.selectedRange() == std::optional<practice::RangeSelection>({
                                       .startMicros = 1'000'000,
                                       .endMicros = 3'000'000,
                                       .active = practice::Marker::Start,
                                   }),
          "reversed drag snaps to exact original measure boundaries");

  model.selectSection(50, 51);
  require(!model.selectedRange().has_value(),
          "out-of-range section selection is cleared");
}

} // namespace

int main() {
  testAggregatesOnlyCompatibleCompletedAttempts();
  testAttemptSelectionAndAutoLabels();
  testSectionSelectionUsesExactMeasureBoundaries();
  return 0;
}
