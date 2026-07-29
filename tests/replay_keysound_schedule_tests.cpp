#include "scene/play/ReplayKeysoundSchedule.h"

#include "ReplayData.h"
#include "bms_parser.hpp"
#include "scene/play/GameplayDefinition.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bms_parser::TimeLine *addTimeline(bms_parser::Measure &measure,
                                  long long timingMicros) {
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = timingMicros;
  measure.TimeLines.push_back(timeline);
  return timeline;
}

gameplay::GameplayDefinition makeDefinition() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 3;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  addTimeline(*measure, 1'000'000)->SetNote(1, new bms_parser::Note(42));
  auto *second = addTimeline(*measure, 2'000'000);
  second->SetNote(1, new bms_parser::Note(84));
  second->SetNote(2, new bms_parser::Note(bms_parser::Parser::NoWav));
  chart.Measures.push_back(measure);
  return gameplay::buildGameplayDefinition(chart, 0);
}

void testReplayPressSchedulesKeysoundAtRawSongTime() {
  const auto definition = makeDefinition();
  const std::array events{
      ReplayEvent{.action = ReplayEventAction::Press,
                  .lane = 1,
                  .noteTimeMicros = 1'000'000,
                  .songTimeMicros = 1'120'000},
      ReplayEvent{.action = ReplayEventAction::Release,
                  .lane = 1,
                  .noteTimeMicros = 1'000'000,
                  .songTimeMicros = 1'220'000}};

  const auto scheduled =
      buildReplayKeysoundSchedule(definition, events, 120'000, std::nullopt);
  require(scheduled.size() == 1 &&
              scheduled[0].timeMicros == 1'000'000 &&
              scheduled[0].wav == 42 &&
              scheduled[0].bus == audio::Bus::Keysound,
          "replay Press is scheduled at raw audio time on the keysound bus");
}

void testReplayNonJudgingPressUsesManualKeysoundPolicy() {
  const auto definition = makeDefinition();
  const std::array events{
      ReplayEvent{.action = ReplayEventAction::Press,
                  .lane = 1,
                  .noteTimeMicros = -1,
                  .songTimeMicros = 620'000,
                  .judgement = None}};

  const auto scheduled =
      buildReplayKeysoundSchedule(definition, events, 120'000, std::nullopt);
  require(scheduled.size() == 1 &&
              scheduled[0].timeMicros == 500'000 &&
              scheduled[0].wav == 42 &&
              scheduled[0].bus == audio::Bus::Keysound,
          "a replay press without judgement uses the same future manual "
          "keysound as live gameplay");
}

void testReplaySchedulePreservesExistingExclusions() {
  const auto definition = makeDefinition();
  const std::array events{
      ReplayEvent{.action = ReplayEventAction::Press,
                  .lane = 9,
                  .noteTimeMicros = 1'000'000,
                  .songTimeMicros = 1'000'000},
      ReplayEvent{.action = ReplayEventAction::Press,
                  .lane = 1,
                  .noteTimeMicros = 9'000'000,
                  .songTimeMicros = 1'000'000},
      ReplayEvent{.action = ReplayEventAction::Press,
                  .lane = 2,
                  .noteTimeMicros = 2'000'000,
                  .songTimeMicros = 2'000'000},
      ReplayEvent{.action = ReplayEventAction::MultiBad,
                  .lane = 1,
                  .noteTimeMicros = 1'000'000,
                  .songTimeMicros = 1'000'000},
      ReplayEvent{.action = ReplayEventAction::Miss,
                  .lane = 1,
                  .noteTimeMicros = 1'000'000,
                  .songTimeMicros = 1'000'000}};

  require(buildReplayKeysoundSchedule(definition, events, 0, std::nullopt)
              .empty(),
          "unresolved, WAV-less, and judgement-only replay events stay "
          "silent");

  const std::array outsideRange{
      ReplayEvent{.action = ReplayEventAction::Press,
                  .lane = 1,
                  .noteTimeMicros = 1'000'000,
                  .songTimeMicros = 1'100'000}};
  require(buildReplayKeysoundSchedule(
              definition, outsideRange, 0,
              gameplay::GameplayTimeRange{.startMicros = 1'500'000,
                                          .endMicros = 3'000'000})
              .empty(),
          "replay keysounds outside the allowed practice range stay silent");
}

void testPracticePreparationPressKeepsItsKeysound() {
  const auto definition = makeDefinition();
  const std::array events{
      ReplayEvent{.action = ReplayEventAction::Press,
                  .lane = 1,
                  .noteTimeMicros = -1,
                  .songTimeMicros = 1'200'000,
                  .judgement = None}};
  const gameplay::GameplayTimeRange practiceRange{.startMicros = 1'500'000,
                                                   .endMicros = 3'000'000};
  const gameplay::GameplayTimeRange preparationRange{
      .startMicros = 1'000'000, .endMicros = 1'500'000};

  const auto scheduled = buildReplayKeysoundSchedule(
      definition, events, 0, practiceRange, preparationRange);
  require(scheduled.size() == 1 && scheduled.front().wav == 84 &&
              scheduled.front().timeMicros == 1'200'000,
          "practice replay preparation uses the same admitted press and "
          "future-range keysound as live input");
  require(replayEventAllowedForPlayback(events.front(), practiceRange,
                                        preparationRange),
          "visual and audio replay paths share preparation admission");
}

} // namespace

int main() {
  testReplayPressSchedulesKeysoundAtRawSongTime();
  testReplayNonJudgingPressUsesManualKeysoundPolicy();
  testReplaySchedulePreservesExistingExclusions();
  testPracticePreparationPressKeepsItsKeysound();
  return 0;
}
