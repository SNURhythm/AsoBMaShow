#include "scene/play/BMSRenderer.h"
#include "scene/play/CompiledGameplayJudge.h"
#include "scene/play/GameplayDefinition.h"
#include "scene/play/GameplaySimulation.h"
#include "scene/play/Judge.h"
#include "scene/play/RhythmLaneInputController.h"

#include "bms_parser.hpp"

#include <cstdlib>
#include <iostream>
#include <unordered_map>

void BMSRenderer::onLanePressed(int, const JudgeResult, long long) {}
void BMSRenderer::onLaneReleased(int, long long) {}

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

void testClassicReleaseCommitsOneJudgeAndNoSound() {
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
          .laneBeamTimeMicros = 2'000'000});
  const auto release = simulation.releaseLane(
      1, {.songTimeMicros = 1'500'000,
          .laneBeamTimeMicros = 2'500'000});

  require(release.noteId == definition.note(press.noteId).pairId,
          "release resolves the held long-note tail");
  require(release.soundNoteId == gameplay::kInvalidNoteId,
          "release does not create an input-triggered keysound");
  require(release.hasJudge && release.judge.judgement == PGreat,
          "classic release commits its combined judgement");
  require(!simulation.noteState(press.noteId).holding &&
              !simulation.noteState(release.noteId).holding,
          "release clears both long-note holding identities");
  require(!simulation.lanePressed(1) &&
              release.replayEvent.action ==
                  gameplay::GameplayReplayAction::Release,
          "release commits lane and replay state together");
}

void testChargeScratchRequiresBackspinRelease() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  addLongNote(*measure, 1'000'000, 1'500'000, 7,
              bms_parser::LongNoteType::ChargeNote);
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  simulation.pressLane(7, {.songTimeMicros = 1'000'000});
  const auto release = simulation.releaseLane(
      7, {.songTimeMicros = 1'500'000}, false);
  require(release.hasJudge && release.judge.judgement == Poor,
          "non-backspin scratch release is Poor");
}

struct NoteIdentity {
  bool present = false;
  int lane = -1;
  long long timingMicros = -1;
  int wav = 0;
  gameplay::NoteKind kind = gameplay::NoteKind::Normal;
  gameplay::LongNoteRule longNoteRule = gameplay::LongNoteRule::None;

  bool operator==(const NoteIdentity &) const = default;
};

gameplay::LongNoteRule
oldLongNoteRule(const bms_parser::LongNote *longNote) {
  if (longNote == nullptr) {
    return gameplay::LongNoteRule::None;
  }
  switch (longNote->GetType()) {
  case bms_parser::LongNoteType::ChargeNote:
    return gameplay::LongNoteRule::Charge;
  case bms_parser::LongNoteType::HellChargeNote:
    return gameplay::LongNoteRule::HellCharge;
  case bms_parser::LongNoteType::Undefined:
  case bms_parser::LongNoteType::LongNote:
    return gameplay::LongNoteRule::Classic;
  }
  return gameplay::LongNoteRule::Classic;
}

NoteIdentity oldNoteIdentity(const bms_parser::Note *note) {
  if (note == nullptr) {
    return {};
  }
  gameplay::NoteKind kind = gameplay::NoteKind::Normal;
  const auto *longNote = dynamic_cast<const bms_parser::LongNote *>(note);
  if (dynamic_cast<const bms_parser::LandmineNote *>(note) != nullptr) {
    kind = gameplay::NoteKind::Landmine;
  } else if (longNote != nullptr) {
    kind = longNote->IsTail() ? gameplay::NoteKind::LongTail
                              : gameplay::NoteKind::LongHead;
  }
  return {
      .present = true,
      .lane = note->Lane,
      .timingMicros = note->Timeline == nullptr ? -1 : note->Timeline->Timing,
      .wav = note->Wav,
      .kind = kind,
      .longNoteRule = oldLongNoteRule(longNote),
  };
}

NoteIdentity newNoteIdentity(const gameplay::GameplayDefinition &definition,
                             gameplay::NoteId id) {
  if (id == gameplay::kInvalidNoteId) {
    return {};
  }
  const auto &note = definition.note(id);
  return {
      .present = true,
      .lane = note.lane,
      .timingMicros = note.timingMicros,
      .wav = note.wav,
      .kind = note.kind,
      .longNoteRule = note.longNoteRule,
  };
}

struct JudgeSummary {
  bool present = false;
  Judgement judgement = None;
  long long diffMicros = 0;

  bool operator==(const JudgeSummary &) const = default;
};

enum class ReplayActionSummary { Press, Release, Miss, Mine, Gauge };

ReplayActionSummary replayActionSummary(ReplayEventAction action) {
  switch (action) {
  case ReplayEventAction::Press:
    return ReplayActionSummary::Press;
  case ReplayEventAction::Release:
    return ReplayActionSummary::Release;
  case ReplayEventAction::Miss:
    return ReplayActionSummary::Miss;
  case ReplayEventAction::Mine:
    return ReplayActionSummary::Mine;
  case ReplayEventAction::Gauge:
    return ReplayActionSummary::Gauge;
  }
  return ReplayActionSummary::Press;
}

ReplayActionSummary
replayActionSummary(gameplay::GameplayReplayAction action) {
  switch (action) {
  case gameplay::GameplayReplayAction::Press:
    return ReplayActionSummary::Press;
  case gameplay::GameplayReplayAction::Release:
    return ReplayActionSummary::Release;
  case gameplay::GameplayReplayAction::Miss:
    return ReplayActionSummary::Miss;
  case gameplay::GameplayReplayAction::Mine:
    return ReplayActionSummary::Mine;
  case gameplay::GameplayReplayAction::Gauge:
    return ReplayActionSummary::Gauge;
  }
  return ReplayActionSummary::Press;
}

struct ReplaySummary {
  bool present = false;
  ReplayActionSummary action = ReplayActionSummary::Press;
  int lane = -1;
  NoteIdentity note;
  long long noteTimeMicros = -1;
  long long songTimeMicros = 0;
  long long judgeTimeMicros = 0;
  Judgement judgement = None;
  long long diffMicros = 0;

  bool operator==(const ReplaySummary &) const = default;
};

ReplaySummary oldReplaySummary(
    bool present,
    const RhythmLaneInputController::ReplayEventResult &replay) {
  if (!present) {
    return {};
  }
  const auto note = oldNoteIdentity(replay.note);
  return {
      .present = true,
      .action = replayActionSummary(replay.action),
      .lane = replay.lane,
      .note = note,
      .noteTimeMicros = note.timingMicros,
      .songTimeMicros = replay.songTimeMicros,
      .judgeTimeMicros = replay.judgeTimeMicros,
      .judgement = replay.judge.judgement,
      .diffMicros = replay.judge.Diff,
  };
}

ReplaySummary newReplaySummary(
    bool present, const gameplay::GameplayReplayEvent &replay,
    const gameplay::GameplayDefinition &definition) {
  if (!present) {
    return {};
  }
  return {
      .present = true,
      .action = replayActionSummary(replay.action),
      .lane = replay.lane,
      .note = newNoteIdentity(definition, replay.noteId),
      .noteTimeMicros = replay.noteTimeMicros,
      .songTimeMicros = replay.songTimeMicros,
      .judgeTimeMicros = replay.judgeTimeMicros,
      .judgement = replay.judgement,
      .diffMicros = replay.diffMicros,
  };
}

struct PressSummary {
  NoteIdentity selected;
  NoteIdentity sound;
  JudgeSummary judge;
  ReplaySummary replay;

  bool operator==(const PressSummary &) const = default;
};

PressSummary oldPress(long long diffMicros,
                      AppSettings::NotePriorityMode priority) {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(1, new bms_parser::Note(5));
  chart.Measures.push_back(measure);
  std::unordered_map<int, bool> lanes{{1, false}};
  RhythmLaneInputController controller(&chart, nullptr, lanes, Judge(1));
  const auto result = controller.pressLane(
      1, {.songTimeMicros = 1'000'000 + diffMicros,
          .laneBeamTimeMicros = 2'000'000,
          .notePriorityMode = priority});
  return {
      .selected = oldNoteIdentity(result.note),
      .sound = oldNoteIdentity(result.keySoundNote),
      .judge = {.present = result.hasJudge,
                .judgement = result.judge.judgement,
                .diffMicros = result.judge.Diff},
      .replay = oldReplaySummary(result.hasReplayEvent, result.replayEvent),
  };
}

PressSummary newPress(long long diffMicros,
                      AppSettings::NotePriorityMode priority) {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(1, new bms_parser::Note(5));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .notePriorityMode = priority});
  const auto result = simulation.pressLane(
      1, {.songTimeMicros = 1'000'000 + diffMicros,
          .laneBeamTimeMicros = 2'000'000});
  PressSummary summary{
      .selected = newNoteIdentity(definition, result.noteId),
      .sound = newNoteIdentity(definition, result.soundNoteId),
      .judge = {.present = result.hasJudge,
                .judgement = result.judge.judgement,
                .diffMicros = result.judge.Diff},
      .replay =
          newReplaySummary(result.hasReplayEvent, result.replayEvent,
                           definition),
  };
  return summary;
}

void testParitySummaryDetectsPerturbedIdentityAndPayload() {
  const auto baseline =
      oldPress(0, AppSettings::NotePriorityMode::Lowest);

  auto wrongIdentity = baseline;
  ++wrongIdentity.selected.wav;
  require(wrongIdentity != baseline,
          "parity summary detects a perturbed selected-note identity");

  auto wrongSound = baseline;
  ++wrongSound.sound.timingMicros;
  require(wrongSound != baseline,
          "parity summary detects a perturbed sound-note identity");

  auto wrongJudge = baseline;
  ++wrongJudge.judge.diffMicros;
  require(wrongJudge != baseline,
          "parity summary detects a perturbed JudgeResult diff");

  auto wrongReplay = baseline;
  ++wrongReplay.replay.lane;
  require(wrongReplay != baseline,
          "parity summary detects a perturbed replay payload");
}

void testCurrentPressParityMatrix() {
  for (const auto priority : {
           AppSettings::NotePriorityMode::Lowest,
           AppSettings::NotePriorityMode::Duration,
           AppSettings::NotePriorityMode::Combo,
           AppSettings::NotePriorityMode::Score}) {
    for (const long long diff : {-500'001LL, -500'000LL, -30'000LL, 0LL,
                                 30'000LL, 420'000LL, 420'001LL}) {
      const auto oldResult = oldPress(diff, priority);
      const auto newResult = newPress(diff, priority);
      require(oldResult == newResult,
              "new press transaction matches current controller outcome");
    }
  }
}

struct ReleaseSummary {
  NoteIdentity selected;
  NoteIdentity sound;
  JudgeSummary judge;
  ReplaySummary replay;
  bool headHolding = false;
  bool tailHolding = false;

  bool operator==(const ReleaseSummary &) const = default;
};

ReleaseSummary oldRelease(bms_parser::LongNoteType type, int lane,
                          bool isBackSpin, long long diffMicros) {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  auto *head = addLongNote(*measure, 1'000'000, 1'500'000, lane, type);
  chart.Measures.push_back(measure);
  std::unordered_map<int, bool> lanes;
  RhythmLaneInputController controller(&chart, nullptr, lanes, Judge(1));
  head->Press(1'000'000);
  lanes[lane] = true;
  const auto result = controller.releaseLane(
      lane, {.songTimeMicros = 1'500'000 + diffMicros,
             .laneBeamTimeMicros = 2'000'000},
      isBackSpin);
  return {
      .selected = oldNoteIdentity(result.note),
      .sound = oldNoteIdentity(result.keySoundNote),
      .judge = {.present = result.hasJudge,
                .judgement = result.judge.judgement,
                .diffMicros = result.judge.Diff},
      .replay = oldReplaySummary(result.hasReplayEvent, result.replayEvent),
      .headHolding = head->IsHolding,
      .tailHolding = head->Tail->IsHolding,
  };
}

ReleaseSummary newRelease(bms_parser::LongNoteType type, int lane,
                          bool isBackSpin, long long diffMicros) {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  addLongNote(*measure, 1'000'000, 1'500'000, lane, type);
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto press = simulation.pressLane(
      lane, {.songTimeMicros = 1'000'000,
             .laneBeamTimeMicros = 1'500'000});
  const auto tailId = definition.note(press.noteId).pairId;
  const auto result = simulation.releaseLane(
      lane, {.songTimeMicros = 1'500'000 + diffMicros,
             .laneBeamTimeMicros = 2'000'000},
      isBackSpin);
  return {
      .selected = newNoteIdentity(definition, result.noteId),
      .sound = newNoteIdentity(definition, result.soundNoteId),
      .judge = {.present = result.hasJudge,
                .judgement = result.judge.judgement,
                .diffMicros = result.judge.Diff},
      .replay =
          newReplaySummary(result.hasReplayEvent, result.replayEvent,
                           definition),
      .headHolding = simulation.noteState(press.noteId).holding,
      .tailHolding = simulation.noteState(tailId).holding,
  };
}

void testCurrentReleaseParityMatrix() {
  struct ReleaseCase {
    bms_parser::LongNoteType type;
    int lane;
    bool isBackSpin;
    long long diffMicros;
  };
  for (const auto &entry : {
           ReleaseCase{bms_parser::LongNoteType::LongNote, 1, false, -30'000},
           ReleaseCase{bms_parser::LongNoteType::LongNote, 1, false, 0},
           ReleaseCase{bms_parser::LongNoteType::ChargeNote, 1, false, 30'000},
           ReleaseCase{bms_parser::LongNoteType::HellChargeNote, 1, false, 0},
           ReleaseCase{bms_parser::LongNoteType::ChargeNote, 7, false, 0},
           ReleaseCase{bms_parser::LongNoteType::ChargeNote, 7, true, 0}}) {
    const auto oldResult =
        oldRelease(entry.type, entry.lane, entry.isBackSpin, entry.diffMicros);
    const auto newResult =
        newRelease(entry.type, entry.lane, entry.isBackSpin, entry.diffMicros);
    require(oldResult == newResult,
            "new release transaction matches current controller outcome");
  }
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
  testClassicReleaseCommitsOneJudgeAndNoSound();
  testChargeScratchRequiresBackspinRelease();
  testParitySummaryDetectsPerturbedIdentityAndPayload();
  testCurrentPressParityMatrix();
  testCurrentReleaseParityMatrix();
  return 0;
}
