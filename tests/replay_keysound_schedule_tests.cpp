#include "scene/play/ReplayKeysoundSchedule.h"

#include "ReplayData.h"
#include "bms_parser.hpp"
#include "scene/play/GameplayDefinition.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>

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

void testBmsonMineReplayUsesAuthoredSoundOnlyOnDetonation() {
  const std::string input = R"({"version":"1.0.0","info":{"init_bpm":120},
    "sound_channels":[{"name":"normal.wav","notes":[{"x":1,"y":480}]}],
    "mine_channels":[{"name":"mine.wav","notes":[{"x":2,"y":480,"damage":4}]}]})";
  bms_parser::Parser parser;
  bms_parser::Chart *raw = nullptr;
  std::atomic_bool cancelled{false};
  parser.Parse(std::vector<unsigned char>(input.begin(), input.end()), &raw,
               false, false, cancelled, "mine.bmson");
  const std::unique_ptr<bms_parser::Chart> chart(raw);
  require(chart && chart->Meta.TotalNotes == 1 && chart->Meta.TotalLandmineNotes == 1,
          "bmson fixture has a normal note and distinct mine at the same time");
  const auto definition = gameplay::buildGameplayDefinition(*chart, 0);
  const std::array events{
      ReplayEvent{.action = ReplayEventAction::Press, .lane = 0,
                  .noteTimeMicros = 1'000'000, .songTimeMicros = 1'100'000},
      ReplayEvent{.action = ReplayEventAction::Mine, .lane = 1,
                  .noteTimeMicros = 1'000'000, .songTimeMicros = 1'120'000}};
  const auto resolved = resolveReplayKeysounds(definition, events, std::nullopt);
  require(resolved.size() == 2 &&
              resolved[0].wav == definition.note(0).wav &&
              resolved[0].wav != resolved[1].wav &&
              chart->WavTable.at(resolved[1].wav) == "mine.wav" &&
              resolved[1].songTimeMicros == 1'120'000,
          "live and offline replay resolve Mine to its authored sound, not a simultaneous normal note");
  const auto scheduled = buildReplayKeysoundSchedule(definition, events, 120'000, std::nullopt);
  require(scheduled.size() == 2 && scheduled[1].timeMicros == 1'000'000 &&
              scheduled[1].wav == resolved[1].wav && scheduled[1].bus == audio::Bus::Keysound,
          "mine replay sound uses the keysound bus and compensated detonation time");
  require(resolveReplayKeysounds(definition, std::span(events).first(1), std::nullopt).size() == 1,
          "a normal press does not also play the simultaneous mine");
  auto missing = events[1];
  missing.noteTimeMicros = -1;
  require(resolveReplayKeysounds(definition, std::span(&missing, 1), std::nullopt).empty(),
          "an unresolved Mine event must not fall back to a manual normal keysound");
  require(resolveReplayKeysounds(definition, events,
              gameplay::GameplayTimeRange{.startMicros = 1'500'000, .endMicros = 2'000'000}).empty(),
          "practice range excludes mine sounds together with their events");
}

} // namespace

int main() {
  testReplayPressSchedulesKeysoundAtRawSongTime();
  testReplayNonJudgingPressUsesManualKeysoundPolicy();
  testReplaySchedulePreservesExistingExclusions();
  testPracticePreparationPressKeepsItsKeysound();
  testBmsonMineReplayUsesAuthoredSoundOnlyOnDetonation();
  return 0;
}
