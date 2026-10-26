#include "scene/play/ReplayKeysoundSchedule.h"

#include "ReplayData.h"
#include "BmsonReleaseFixture.h"
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

void testEmptyPressesBeforeLongNoteRetainEveryTrigger() {
  for (const int type : {1, 2, 3}) {
    auto chart = bmsonReleaseFixture(type, 1, false, 960);
    const auto definition = gameplay::buildGameplayDefinition(*chart, 0);
    const std::array events{
        ReplayEvent{.action = ReplayEventAction::Press, .lane = 0,
                    .songTimeMicros = 100'000},
        ReplayEvent{.action = ReplayEventAction::Release, .lane = 0,
                    .songTimeMicros = 150'000},
        ReplayEvent{.action = ReplayEventAction::Press, .lane = 0,
                    .songTimeMicros = 200'000}};
    const auto sounds = resolveReplayKeysounds(definition, events, std::nullopt);
    require(sounds.size() == 2 && !sounds[0].setVoiceGain && !sounds[1].setVoiceGain &&
                sounds[0].songTimeMicros == 100'000 && sounds[1].songTimeMicros == 200'000 &&
                sounds[0].voiceId != sounds[1].voiceId,
            "selecting a future long-note keysound does not enter hold recovery");
  }
}

void testLateFailedReleaseMutesHead() {
  for (const int type : {2, 3}) {
    auto chart = bmsonReleaseFixture(type, 1, false, 960);
    const auto definition = gameplay::buildGameplayDefinition(*chart, 0);
    const std::array events{
        ReplayEvent{.action = ReplayEventAction::Press, .lane = 0,
                    .noteTimeMicros = 1'000'000, .songTimeMicros = 1'000'000,
                    .judgement = PGreat},
        ReplayEvent{.action = ReplayEventAction::Release, .lane = 0,
                    .noteTimeMicros = 3'000'000, .songTimeMicros = 3'180'000,
                    .judgement = Bad, .diffMicros = 180'000}};
    const auto sounds = resolveReplayKeysounds(definition, events, std::nullopt);
    require(sounds.size() == 2 && sounds[1].setVoiceGain && sounds[1].gain == 0.0F &&
                sounds[1].voiceId == sounds[0].voiceId &&
                sounds[1].songTimeMicros == 3'180'000,
            "late BAD CN/HCN lifts mute the ongoing head in replay");
  }
}

void testFailedBackspinKeepsHeadVoice() {
  auto chart = bmsonReleaseFixture(3, 8, false, 960);
  const auto definition = gameplay::buildGameplayDefinition(*chart, 0);
  std::vector<ReplayEvent> events{
      {.action = ReplayEventAction::Press, .lane = 7,
       .noteTimeMicros = 1'000'000, .songTimeMicros = 1'000'000, .judgement = PGreat},
      {.action = ReplayEventAction::Release, .lane = 7,
       .noteTimeMicros = 3'000'000, .songTimeMicros = 1'500'000,
       .judgement = Poor, .diffMicros = -1'500'000, .releaseByBackspin = true},
      {.action = ReplayEventAction::Release, .lane = 7,
       .songTimeMicros = 1'700'000},
      {.action = ReplayEventAction::Release, .lane = 7,
       .songTimeMicros = 1'800'000},
      {.action = ReplayEventAction::Press, .lane = 7,
       .songTimeMicros = 2'000'000}};
  const auto schedule = buildReplayKeysoundSchedule(definition, events, 100'000, std::nullopt);
  require(schedule.size() == 3 && !schedule[0].setVoiceGain &&
              schedule[1].setVoiceGain && schedule[1].gain == 0 && schedule[1].timeMicros == 1'600'000 &&
              schedule[2].setVoiceGain && schedule[2].gain == 1 && schedule[2].timeMicros == 1'900'000 &&
              schedule[0].voiceId == schedule[1].voiceId && schedule[1].voiceId == schedule[2].voiceId,
          "failed scratch reversal keeps head audible; later HCN lift/rehold controls once at raw audio times");
}

void testBmsonReleaseReplaySound() {
  for (int type : {1, 2, 3}) for (bool tailSound : {false, true}) {
    auto chart = bmsonReleaseFixture(type, 1, tailSound);
    const auto definition = gameplay::buildGameplayDefinition(*chart, 0);
    for (auto judgement : {PGreat, Bad, Poor, None}) {
      const std::array events{ReplayEvent{
          .action = ReplayEventAction::Release, .lane = 0,
          .noteTimeMicros = 2'000'000, .songTimeMicros = 1'700'000,
          .judgement = judgement}};
      const auto live = resolveReplayKeysounds(definition, events, std::nullopt);
      const auto offline = buildReplayKeysoundSchedule(definition, events, 100'000, std::nullopt);
      const bool sounds = tailSound && judgement != None;
      require(live.size() == (sounds ? 1 : 0) && offline.size() == live.size(),
              "only resolved authored releases sound in live and offline replay");
      if (sounds) require(chart->WavTable.at(chart->WavSlices.at(live[0].wav)[0].SourceWav) == "tail.wav" &&
                  live[0].songTimeMicros == 1'700'000 && offline[0].timeMicros == 1'600'000 &&
                  offline[0].wav == live[0].wav && offline[0].bus == audio::Bus::Keysound,
              "replay uses tail audio at resolution time, including BAD/POOR releases");
      auto invalid = events[0];
      for (auto time : {-1LL, 1'000'000LL}) {
        invalid.noteTimeMicros = time;
        require(resolveReplayKeysounds(definition, std::span(&invalid, 1), std::nullopt).empty(),
                "unresolved release and head identity must not fall back to a press sound");
      }
      require(resolveReplayKeysounds(definition, events,
                  gameplay::GameplayTimeRange{.startMicros = 1'800'000, .endMicros = 3'000'000}).empty(),
              "release audio respects replay admission range");
    }
  }
}

void testAcceptedHoldEndsAtRecordedResolution() {
  auto chart = bmsonReleaseWithSentinelFixture();
  const auto definition = gameplay::buildGameplayDefinition(*chart, 0);
  for (const auto pressTime : {1'999'999LL, 2'000'000LL, 2'000'001LL, 2'000'002LL}) {
    std::vector<ReplayEvent> events{
        {.action = ReplayEventAction::Press, .lane = 7,
         .noteTimeMicros = 1'000'000, .songTimeMicros = 1'000'000, .judgement = PGreat},
        {.action = ReplayEventAction::Press, .lane = 7, .songTimeMicros = pressTime},
        {.action = ReplayEventAction::Release, .lane = 7,
         .noteTimeMicros = 2'000'000, .songTimeMicros = 2'000'001,
         .judgement = PGreat, .releaseByHolding = true}};
    if (pressTime > 2'000'001) std::swap(events[1], events[2]);
    const auto sounds = resolveReplayKeysounds(definition, events, std::nullopt);
    require(sounds.size() == (pressTime > 2'000'001 ? 3 : 2),
            "accepted classic hold suppresses empty presses until its recorded completion");
    require(sounds[1].wav == definition.note(1).wav && sounds[1].songTimeMicros == 2'000'001,
            "held classic completion still plays its authored tail at resolution");
  }
  for (bool releaseFirst : {false, true}) {
    std::vector<ReplayEvent> events{
        {.action = ReplayEventAction::Press, .lane = 7,
         .noteTimeMicros = 1'000'000, .songTimeMicros = 1'000'000, .judgement = PGreat},
        {.action = ReplayEventAction::Press, .lane = 7, .songTimeMicros = 2'000'000},
        {.action = ReplayEventAction::Release, .lane = 7,
         .noteTimeMicros = 2'000'000, .songTimeMicros = 2'000'000, .judgement = PGreat}};
    if (releaseFirst) std::swap(events[1], events[2]);
    require(resolveReplayKeysounds(definition, events, std::nullopt).size() == (releaseFirst ? 3 : 2),
            "same-time scratch handoff respects recorded Release/Press order");
  }
}

void testBackspinCompanionSuppressionBoundaries() {
  for (const auto ruleset : {GameplayRuleset::LR2, GameplayRuleset::Beatoraja})
    for (const int type : {1, 2, 3}) for (const bool cause : {false, true})
      for (const bool tailSound : {false, true})
      for (const auto delta : {-1LL, 0LL, 1LL}) {
        auto chart = bmsonReleaseWithSentinelFixture(type, tailSound);
        const auto definition = gameplay::buildGameplayDefinition(*chart, 0);
        std::vector<ReplayEvent> events{
            {.action = ReplayEventAction::Press, .lane = 7,
             .noteTimeMicros = 1'000'000, .songTimeMicros = 1'000'000, .judgement = PGreat},
            {.action = ReplayEventAction::Release, .lane = 7,
             .noteTimeMicros = 2'000'000, .songTimeMicros = 2'000'000,
             .judgement = PGreat, .releaseByBackspin = cause},
            {.action = ReplayEventAction::Press, .lane = 7, .songTimeMicros = 2'000'000 + delta}};
        if (delta < 0) std::swap(events[1], events[2]);
        const auto sounds = resolveReplayKeysounds(definition, events, std::nullopt, std::nullopt, ruleset);
        const bool suppressed = delta < 0 ||
            (delta == 0 && cause && ruleset == GameplayRuleset::LR2 && type != 1);
        require(sounds.size() == (suppressed ? 1 : 2) + tailSound &&
                    std::ranges::count(sounds, definition.note(1).wav, &ReplayKeysoundEvent::wav) == tailSound,
                "only the LR2 nonclassic same-time companion is silent; classic, Beatoraja, later and no-cause presses remain audible");
      }
  auto chart = bmsonReleaseWithSentinelFixture(2);
  const auto definition = gameplay::buildGameplayDefinition(*chart, 0);
  std::vector<ReplayEvent> events{
      {.action = ReplayEventAction::Press, .lane = 7,
       .noteTimeMicros = 1'000'000, .songTimeMicros = 1'000'000, .judgement = PGreat},
      {.action = ReplayEventAction::Release, .lane = 7,
       .noteTimeMicros = 2'000'000, .songTimeMicros = 2'000'000,
       .judgement = PGreat, .releaseByBackspin = true},
      {.action = ReplayEventAction::Press, .lane = 0, .songTimeMicros = 2'000'000},
      {.action = ReplayEventAction::Press, .lane = 7, .songTimeMicros = 2'000'000},
      {.action = ReplayEventAction::Release, .lane = 7, .songTimeMicros = 2'000'000},
      {.action = ReplayEventAction::Press, .lane = 7, .songTimeMicros = 2'000'000}};
  const auto sounds = resolveReplayKeysounds(definition, events, std::nullopt, std::nullopt, GameplayRuleset::LR2);
  require(sounds.size() == 3 && sounds.back().wav == definition.note(0).wav,
          "companion suppression is lane-local and consumed exactly once, allowing a subsequent physical press");
  for (const auto [mode, lane] : std::array<std::pair<int, int>, 3>{{{7, 1}, {24, 24}, {48, 50}}}) {
    auto nonDirectional = bmsonReleaseWithSentinelFixture(2);
    nonDirectional->Meta.KeyMode = mode;
    for (auto *measure : nonDirectional->Measures) for (auto *timeline : measure->TimeLines) {
      if (auto *note = timeline->Notes[7]) {
        timeline->Notes[7] = nullptr;
        timeline->Notes.resize(std::max<std::size_t>(timeline->Notes.size(), lane + 1));
        timeline->SetNote(lane, note);
      }
    }
    const auto otherDefinition = gameplay::buildGameplayDefinition(*nonDirectional, 0);
    auto otherEvents = std::vector(events.begin(), events.begin() + 2);
    otherEvents.push_back({.action = ReplayEventAction::Press, .songTimeMicros = 2'000'000});
    for (auto &event : otherEvents) event.lane = lane;
    require(resolveReplayKeysounds(otherDefinition, otherEvents, std::nullopt,
                                   std::nullopt, GameplayRuleset::LR2).size() == 3,
            "normal keys and independent keyboard wheels do not inherit directional scratch companion suppression");
  }
}

void testTailMissEndsAcceptedHoldWithoutPlayingTail() {
  for (const int type : {1, 2, 3}) {
    auto chart = bmsonReleaseWithSentinelFixture(type);
    const auto definition = gameplay::buildGameplayDefinition(*chart, 0);
    const std::array events{
        ReplayEvent{.action = ReplayEventAction::Press, .lane = 7,
                    .noteTimeMicros = 1'000'000, .songTimeMicros = 1'000'000, .judgement = PGreat},
        ReplayEvent{.action = ReplayEventAction::Miss, .lane = 7,
                    .noteTimeMicros = 2'000'000, .songTimeMicros = 2'300'000, .judgement = Poor},
        ReplayEvent{.action = ReplayEventAction::Release, .lane = 7, .songTimeMicros = 2'400'000},
        ReplayEvent{.action = ReplayEventAction::Press, .lane = 7, .songTimeMicros = 2'500'000}};
    const auto sounds = resolveReplayKeysounds(definition, events, std::nullopt);
    require(sounds.size() == 2 && !sounds.back().setVoiceGain && sounds.back().songTimeMicros == 2'500'000,
            "tail Miss clears accepted hold for a later legitimate press and emits no release sound");
  }
}

} // namespace

int main() {
  testBackspinCompanionSuppressionBoundaries();
  testAcceptedHoldEndsAtRecordedResolution();
  testTailMissEndsAcceptedHoldWithoutPlayingTail();
  testEmptyPressesBeforeLongNoteRetainEveryTrigger();
  testLateFailedReleaseMutesHead();
  testFailedBackspinKeepsHeadVoice();
  testBmsonReleaseReplaySound();
  testReplayPressSchedulesKeysoundAtRawSongTime();
  testReplayNonJudgingPressUsesManualKeysoundPolicy();
  testReplaySchedulePreservesExistingExclusions();
  testPracticePreparationPressKeepsItsKeysound();
  testBmsonMineReplayUsesAuthoredSoundOnlyOnDetonation();
  return 0;
}
