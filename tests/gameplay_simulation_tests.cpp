#include "scene/play/CompiledGameplayJudge.h"
#include "scene/play/GameplayDefinition.h"
#include "scene/play/GameplaySimulation.h"
#include "scene/play/Judge.h"

#include "bms_parser.hpp"

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

bms_parser::LongNote *addLongNote(bms_parser::Measure &measure,
                                  long long headMicros,
                                  long long tailMicros, int lane,
                                  bms_parser::LongNoteType type) {
  auto *headTimeline = addTimeline(measure, headMicros);
  auto *tailTimeline = addTimeline(measure, tailMicros);
  auto *head = new bms_parser::LongNote(7, type);
  auto *tail = new bms_parser::LongNote(7, type);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(lane, head);
  tailTimeline->SetNote(lane, tail);
  return head;
}

void testCompiledJudgePreservesResolvedWindows() {
  Judge judge(1);
  judge.applyWindowScale(50, 200);
  const auto compiled = gameplay::CompiledGameplayJudge::from(judge);

  require(compiled.judgeAt(1'000'000, 1'010'000).judgement == PGreat,
          "compiled judge preserves the resolved PGreat window");
  require(compiled.judgeAt(1'000'000, 1'030'000).judgement == Great,
          "compiled judge preserves the resolved Great window");
  require(compiled.window(Bad)->lateMicros == 420'000,
          "compiled judge exposes the Bad late edge");
  require(compiled.latestHittableNoteTiming(1'000'000) == 1'500'000,
          "future cutoff uses the earliest hittable edge");
}

void testDefinitionUsesStableIdsAndLaneIndices() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *normalTimeline = addTimeline(*measure, 500'000);
  normalTimeline->SetNote(2, new bms_parser::Note(3));
  auto *head = addLongNote(*measure, 700'000, 900'000, 1,
                           bms_parser::LongNoteType::ChargeNote);
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  require(definition.noteCount() == 3,
          "normal and both long-note identities receive stable IDs");
  const auto laneOne = definition.laneNotes(1);
  require(laneOne.size() == 2,
          "lane index contains the long-note head and tail");
  const auto &headDefinition = definition.note(laneOne[0]);
  const auto &tailDefinition = definition.note(laneOne[1]);
  require(headDefinition.kind == gameplay::NoteKind::LongHead &&
              tailDefinition.kind == gameplay::NoteKind::LongTail,
          "long-note identities retain head and tail roles");
  require(headDefinition.pairId == tailDefinition.id &&
              tailDefinition.pairId == headDefinition.id,
          "long-note identities point to each other by stable ID");
  require(headDefinition.longNoteRule == gameplay::LongNoteRule::Charge,
          "effective long-note behavior is compiled once");
  require(definition.note(laneOne[0]).timingMicros ==
              head->Timeline->Timing,
          "definition copies timing without retaining mutable note state");
  require(definition.laneNotes(99).empty(),
          "unknown lanes return an empty span without allocation");
}

void testCandidateSelectionIsLaneIndexed() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  for (int index = 0; index < 1'000; ++index) {
    auto *timeline = addTimeline(*measure, index * 10'000LL);
    timeline->SetNote(2, new bms_parser::Note(1));
  }
  auto *targetTimeline = addTimeline(*measure, 5'000'000);
  targetTimeline->SetNote(1, new bms_parser::Note(9));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .notePriorityMode = AppSettings::NotePriorityMode::Lowest});

  const auto result = simulation.pressLane(
      1, 1, {.songTimeMicros = 5'000'000,
             .laneBeamTimeMicros = 7'000'000});
  require(result.noteId != gameplay::kInvalidNoteId,
          "target lane resolves its note");
  require(simulation.lastSearchStats().notesExamined <= 2,
          "unrelated lanes are not scanned");
}

void testCompensationAndPriorityMatchCurrentRules() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *early = addTimeline(*measure, 970'000);
  early->SetNote(1, new bms_parser::Note(1));
  auto *exact = addTimeline(*measure, 1'000'000);
  exact->SetNote(2, new bms_parser::Note(2));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .notePriorityMode = AppSettings::NotePriorityMode::Duration});
  const auto result = simulation.pressLane(
      1, 2, {.songTimeMicros = 1'000'000,
             .laneBeamTimeMicros = 2'000'000});
  require(definition.note(result.noteId).lane == 2,
          "duration priority selects the closer compensation-lane note");
}

void testPracticeRangeIsHalfOpenBeforeLaneMutation() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *inside = addTimeline(*measure, 999'999);
  inside->SetNote(1, new bms_parser::Note(1));
  auto *atEnd = addTimeline(*measure, 1'000'000);
  atEnd->SetNote(2, new bms_parser::Note(2));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .allowedNoteRange = gameplay::GameplayTimeRange{
           .startMicros = 500'000, .endMicros = 1'000'000}});
  const auto accepted = simulation.pressLane(
      1, {.songTimeMicros = 999'999, .laneBeamTimeMicros = 2'000'000});
  const auto rejected = simulation.pressLane(
      2, {.songTimeMicros = 1'000'000,
          .laneBeamTimeMicros = 2'000'001});
  require(accepted.noteId != gameplay::kInvalidNoteId,
          "the final microsecond inside practice remains hittable");
  require(rejected.noteId == gameplay::kInvalidNoteId &&
              !simulation.lanePressed(2),
          "the exclusive end blocks selection before lane mutation");
}

void testEqualTimeKeepsMainLanePrecedence() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(1, new bms_parser::Note(1));
  timeline->SetNote(2, new bms_parser::Note(2));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .notePriorityMode = AppSettings::NotePriorityMode::Lowest});
  const auto result = simulation.pressLane(
      2, 1, {.songTimeMicros = 1'000'000,
             .laneBeamTimeMicros = 2'000'000});
  require(definition.note(result.noteId).lane == 2,
          "equal-time compensation keeps the caller's main lane first");
}

int selectedEqualTimeLane(AppSettings::NotePriorityMode priorityMode) {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(1, new bms_parser::Note(1));
  timeline->SetNote(2, new bms_parser::Note(2));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .notePriorityMode = priorityMode});
  const auto result = simulation.pressLane(
      2, 1, {.songTimeMicros = 1'100'000,
             .laneBeamTimeMicros = 2'000'000});
  return definition.note(result.noteId).lane;
}

void testEqualTimeKeepsMainLanePrecedenceForLatePriorityModes() {
  const int comboLane =
      selectedEqualTimeLane(AppSettings::NotePriorityMode::Combo);
  const int scoreLane =
      selectedEqualTimeLane(AppSettings::NotePriorityMode::Score);
  require(comboLane == 2 && scoreLane == 2,
          "equal-time main-lane precedence survives Combo and Score priority");
}

void testReleaseSearchStopsAtPracticeEnd() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  for (int index = 0; index < 1'000; ++index) {
    auto *timeline = addTimeline(*measure, 1'000'000 + index * 10'000LL);
    timeline->SetNote(1, new bms_parser::Note(1));
  }
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .allowedNoteRange = gameplay::GameplayTimeRange{
           .startMicros = 0, .endMicros = 1'000'000}});
  const gameplay::GameplayInputContext context{
      .songTimeMicros = 999'999, .laneBeamTimeMicros = 2'000'000};
  const auto first = simulation.releaseLane(1, context);
  const auto firstNotesExamined = simulation.lastSearchStats().notesExamined;
  const auto second = simulation.releaseLane(1, context);
  const auto secondNotesExamined = simulation.lastSearchStats().notesExamined;

  require(first.noteId == gameplay::kInvalidNoteId &&
              second.noteId == gameplay::kInvalidNoteId,
          "notes at the practice end remain excluded from release selection");
  require(firstNotesExamined <= 1 && secondNotesExamined <= 1,
          "repeated release search does not rescan the excluded practice tail");
}

void testPressCommitsStateAndSoundTogether() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(1, new bms_parser::Note(42));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto first = simulation.pressLane(
      1, {.songTimeMicros = 1'000'000,
          .laneBeamTimeMicros = 9'000'000});

  require(first.noteId != gameplay::kInvalidNoteId &&
              first.soundNoteId == first.noteId,
          "accepted press returns matching note and sound identity");
  require(simulation.noteState(first.noteId).played,
          "accepted press commits note state before returning");
  require(first.hasJudge && first.judge.judgement == PGreat,
          "normal note commits its judgement");
  require(first.hasReplayEvent &&
              first.replayEvent.action == gameplay::GameplayReplayAction::Press,
          "accepted press commits replay intent");
  require(first.hasLaneVisual && simulation.lanePressed(1),
          "accepted press commits lane state and visual intent");

  const auto duplicate = simulation.pressLane(
      1, {.songTimeMicros = 1'000'001,
          .laneBeamTimeMicros = 9'000'001});
  require(duplicate.noteId == gameplay::kInvalidNoteId &&
              duplicate.soundNoteId == gameplay::kInvalidNoteId,
          "held-lane duplicate produces neither note nor sound");
}

void testClassicLongHeadDefersJudgeButStillCommitsSoundAndHolding() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  addLongNote(*measure, 1'000'000, 1'500'000, 1,
              bms_parser::LongNoteType::LongNote);
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});

  const auto press = simulation.pressLane(
      1, {.songTimeMicros = 1'000'000,
          .laneBeamTimeMicros = 3'000'000});
  const auto &head = definition.note(press.noteId);
  require(press.soundNoteId == press.noteId && !press.hasJudge,
          "classic head sounds now and defers scoring to release");
  require(simulation.noteState(head.id).holding &&
              simulation.noteState(head.pairId).holding,
          "classic head atomically marks both identities holding");
}

void testPressDoesNotClaimLongTail() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  addLongNote(*measure, 0, 1'000'000, 1,
              bms_parser::LongNoteType::LongNote);
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto laneNotes = definition.laneNotes(1);
  const auto tailId = laneNotes[1];
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});

  const auto press = simulation.pressLane(
      1, {.songTimeMicros = 1'000'000,
          .laneBeamTimeMicros = 3'000'000});

  require(press.noteId == gameplay::kInvalidNoteId &&
              press.soundNoteId == gameplay::kInvalidNoteId,
          "press near a long tail claims neither note nor sound identity");
  require(!simulation.lanePressed(1) &&
              !simulation.noteState(tailId).played &&
              !simulation.noteState(tailId).holding,
          "long-tail rejection leaves lane and note state unchanged");
  require(!press.hasJudge && !press.hasReplayEvent && !press.hasLaneVisual,
          "long-tail rejection returns no judgement, replay, or visual intent");
}
} // namespace

int main() {
  testCompiledJudgePreservesResolvedWindows();
  testDefinitionUsesStableIdsAndLaneIndices();
  testCandidateSelectionIsLaneIndexed();
  testCompensationAndPriorityMatchCurrentRules();
  testPracticeRangeIsHalfOpenBeforeLaneMutation();
  testEqualTimeKeepsMainLanePrecedence();
  testEqualTimeKeepsMainLanePrecedenceForLatePriorityModes();
  testReleaseSearchStopsAtPracticeEnd();
  testPressCommitsStateAndSoundTogether();
  testClassicLongHeadDefersJudgeButStillCommitsSoundAndHolding();
  testPressDoesNotClaimLongTail();
  return 0;
}
