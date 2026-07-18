#include "scene/play/BMSRenderer.h"
#include "scene/play/CompiledGameplayJudge.h"
#include "scene/play/GameplayDefinition.h"
#include "scene/play/GameplayCandidateRules.h"
#include "scene/play/GameplayJudgeRules.h"
#include "scene/play/GameplayNoteJudgeRole.h"
#include "scene/play/GameplaySimulation.h"
#include "scene/play/Judge.h"
#include "scene/play/ManualKeysoundSelection.h"
#include "scene/play/RhythmLaneInputController.h"

#include "bms_parser.hpp"

#include <algorithm>
#include <array>
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

bool sameAttemptSnapshot(const gameplay::GameplayAttemptSnapshot &left,
                         const gameplay::GameplayAttemptSnapshot &right) {
  return left.judgeCounts == right.judgeCounts &&
         left.combo == right.combo && left.maxCombo == right.maxCombo &&
         left.comboBreak == right.comboBreak && left.score == right.score &&
         left.gauge == right.gauge && left.gaugeType == right.gaugeType &&
         left.clearTypeRank == right.clearTypeRank;
}

void requireSameScoreState(const GameplayScoreState &left,
                           const GameplayScoreState &right) {
  require(left.isPlaying == right.isPlaying &&
              left.isEnding == right.isEnding &&
              left.passedMeasureCount == right.passedMeasureCount &&
              left.passedTimelineCount == right.passedTimelineCount,
          "score lifecycle state matches standalone commits");
  require(left.judgeCount == right.judgeCount &&
              left.judgementFastSlowCount == right.judgementFastSlowCount &&
              left.combo == right.combo && left.maxCombo == right.maxCombo &&
              left.comboBreak == right.comboBreak &&
              left.getScore() == right.getScore(),
          "judge, score, and combo state matches standalone commits");
  require(left.gaugeHistory == right.gaugeHistory &&
              left.currentGauge == right.currentGauge &&
              left.gaugeType == right.gaugeType &&
              left.selectedGaugeType == right.selectedGaugeType &&
              left.gaugeAutoShiftLowerBound ==
                  right.gaugeAutoShiftLowerBound &&
              left.gaugeProfile == right.gaugeProfile &&
              left.gaugeAutoShift == right.gaugeAutoShift &&
              left.assistClearMark == right.assistClearMark &&
              left.gaugeValues == right.gaugeValues &&
              left.gaugeSurvivalFailed == right.gaugeSurvivalFailed &&
              left.getClearTypeRank() == right.getClearTypeRank() &&
              left.gaugeHistoryOverflowed() ==
                  right.gaugeHistoryOverflowed(),
          "gauge and clear state matches standalone commits");
  require(left.fastCount == right.fastCount &&
              left.slowCount == right.slowCount,
          "fast and slow totals match standalone commits");
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

struct KeysoundEntry {
  std::int64_t timingMicros = 0;
  int identity = 0;
};

void testManualKeysoundSelectionUsesFutureThenLastWithMainTies() {
  const std::array main{
      KeysoundEntry{.timingMicros = 100, .identity = 1},
      KeysoundEntry{.timingMicros = 300, .identity = 3},
  };
  const std::array compensation{
      KeysoundEntry{.timingMicros = 200, .identity = 2},
      KeysoundEntry{.timingMicros = 300, .identity = 4},
  };
  const auto timing = [](const KeysoundEntry &entry) {
    return entry.timingMicros;
  };

  const auto between = gameplay::selectManualKeysound(
      std::span<const KeysoundEntry>(main),
      std::span<const KeysoundEntry>(compensation), 150, 0, 1'000, timing);
  require(between.lane == gameplay::ManualKeysoundLane::Compensation &&
              between.index == 0,
          "the earliest future candidate wins across candidate lanes");

  const auto equalTime = gameplay::selectManualKeysound(
      std::span<const KeysoundEntry>(main),
      std::span<const KeysoundEntry>(compensation), 250, 0, 1'000, timing);
  require(equalTime.lane == gameplay::ManualKeysoundLane::Main &&
              equalTime.index == 1,
          "the main lane wins an equal-time future candidate");

  const auto afterAll = gameplay::selectManualKeysound(
      std::span<const KeysoundEntry>(main),
      std::span<const KeysoundEntry>(compensation), 400, 0, 1'000, timing);
  require(afterAll.lane == gameplay::ManualKeysoundLane::Main &&
              afterAll.index == 1,
          "the latest past candidate wins and keeps main-lane ties");

  const auto rangeFiltered = gameplay::selectManualKeysound(
      std::span<const KeysoundEntry>(main),
      std::span<const KeysoundEntry>(compensation), 0, 150, 250, timing);
  require(rangeFiltered.lane ==
                  gameplay::ManualKeysoundLane::Compensation &&
              rangeFiltered.index == 0,
          "half-open range filtering happens before future selection");

  const auto emptyRange = gameplay::selectManualKeysound(
      std::span<const KeysoundEntry>(main),
      std::span<const KeysoundEntry>(compensation), 0, 250, 250, timing);
  require(!emptyRange, "an empty half-open range has no keysound source");
}

void testDefinitionKeysoundIndexExcludesTailsAndLandmines() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *head = addLongNote(*measure, 1'000'000, 2'000'000, 1,
                           bms_parser::LongNoteType::ChargeNote);
  auto *mineTimeline = addTimeline(*measure, 2'500'000);
  mineTimeline->SetLandmineNote(1, new bms_parser::LandmineNote(5.0F));
  auto *normalTimeline = addTimeline(*measure, 3'000'000);
  auto *normal = new bms_parser::Note(9);
  normalTimeline->SetNote(1, normal);
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto keysounds = definition.laneKeysoundNotes(1);
  require(keysounds.size() == 2 &&
              definition.note(keysounds[0]).kind ==
                  gameplay::NoteKind::LongHead &&
              definition.note(keysounds[1]).kind ==
                  gameplay::NoteKind::Normal &&
              definition.note(keysounds[0]).timingMicros ==
                  head->Timeline->Timing &&
              definition.note(keysounds[1]).timingMicros ==
                  normal->Timeline->Timing,
          "keysound index includes pressable notes and excludes tails and "
          "landmines");
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
  const auto laneOneKeysounds = definition.laneKeysoundNotes(1);
  require(laneOneKeysounds.size() == 1 &&
              laneOneKeysounds.front() == headDefinition.id,
          "press keysound index includes the long head but excludes its tail");
  const auto laneTwoKeysounds = definition.laneKeysoundNotes(2);
  require(laneTwoKeysounds.size() == 1 &&
              definition.note(laneTwoKeysounds.front()).kind ==
                  gameplay::NoteKind::Normal,
          "press keysound index includes normal notes");
  require(definition.laneNotes(99).empty(),
          "unknown lanes return an empty span without allocation");
  require(definition.laneKeysoundNotes(99).empty(),
          "unknown keysound lanes return an empty span without allocation");
}

void testEmptyValidLaneCommitsPressAndReleaseIntents() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 5;
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  bool everyValidLaneIsEmpty = true;
  for (const int lane : chart.Meta.GetTotalLaneIndices()) {
    const auto emptyLane = std::ranges::find(
        definition.lanes(), lane, &gameplay::LaneDefinition::lane);
    everyValidLaneIsEmpty =
        everyValidLaneIsEmpty && emptyLane != definition.lanes().end() &&
        emptyLane->noteIds.empty();
  }
  require(everyValidLaneIsEmpty &&
              std::ranges::is_sorted(definition.lanes(), {},
                                     &gameplay::LaneDefinition::lane),
          "definition retains an empty span for every chart-valid lane");

  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto press = simulation.pressLane(
      3, {.songTimeMicros = 1'000'000,
          .laneBeamTimeMicros = 2'000'000});

  require(simulation.lanePressed(3),
          "empty valid lane press commits pressed state");
  require(press.noteId == gameplay::kInvalidNoteId &&
              press.soundNoteId == gameplay::kInvalidNoteId &&
              !press.hasJudge,
          "empty valid lane press creates no note, sound, or judgement");
  require(press.hasReplayEvent &&
              press.replayEvent.action == gameplay::GameplayReplayAction::Press &&
              press.replayEvent.lane == 3 &&
              press.replayEvent.noteId == gameplay::kInvalidNoteId,
          "empty valid lane press commits lane-only replay intent");
  require(press.hasLaneVisual &&
              press.laneVisual.action == gameplay::LaneVisualAction::Press &&
              press.laneVisual.lane == 3 &&
              press.laneVisual.judge.judgement == None,
          "empty valid lane press commits lane-only visual intent");

  const auto release = simulation.releaseLane(
      3, {.songTimeMicros = 1'100'000,
          .laneBeamTimeMicros = 2'100'000});

  require(!simulation.lanePressed(3),
          "empty valid lane release clears pressed state");
  require(release.noteId == gameplay::kInvalidNoteId &&
              release.soundNoteId == gameplay::kInvalidNoteId &&
              !release.hasJudge,
          "empty valid lane release creates no note, sound, or judgement");
  require(release.hasReplayEvent &&
              release.replayEvent.action ==
                  gameplay::GameplayReplayAction::Release &&
              release.replayEvent.lane == 3 &&
              release.replayEvent.noteId == gameplay::kInvalidNoteId,
          "empty valid lane release commits lane-only replay intent");
  require(release.hasLaneVisual &&
              release.laneVisual.action ==
                  gameplay::LaneVisualAction::Release &&
              release.laneVisual.lane == 3 &&
              release.laneVisual.judge.judgement == None,
          "empty valid lane release commits lane-only visual intent");
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

void testRejectedTransactionsResetLatestSearchStats() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(1, new bms_parser::Note(9));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});

  simulation.pressLane(1, {.songTimeMicros = 1'000'000});
  const bool acceptedPressSearched =
      simulation.lastSearchStats().notesExamined > 0;
  simulation.pressLane(1, {.songTimeMicros = 1'000'001});
  const bool rejectedPressReset =
      simulation.lastSearchStats().notesExamined == 0;

  const auto release =
      simulation.releaseLane(1, {.songTimeMicros = 1'100'000});
  const bool acceptedReleaseSearched =
      simulation.lastSearchStats().notesExamined > 0;
  simulation.releaseLane(1, {.songTimeMicros = 1'100'001});
  const bool rejectedReleaseReset =
      simulation.lastSearchStats().notesExamined == 0;

  require(acceptedPressSearched && rejectedPressReset &&
              release.hasReplayEvent && acceptedReleaseSearched &&
              rejectedReleaseReset,
          "latest search stats reset for press and release early rejections");
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

void testEqualTimeLowestKeepsMainLanePrecedence() {
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
          "Lowest priority keeps equal-time compensation behind the main lane");
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

void testEqualTimeFollowsLatePriorityModes() {
  const int comboLane =
      selectedEqualTimeLane(AppSettings::NotePriorityMode::Combo);
  const int scoreLane =
      selectedEqualTimeLane(AppSettings::NotePriorityMode::Score);
  require(comboLane == 1 && scoreLane == 1,
          "equal-time Combo and Score selection follows current late-note rules");
}

void testReleaseSearchStopsAtPracticeEnd() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *inside = addTimeline(*measure, 999'999);
  inside->SetNote(1, new bms_parser::Note(1));
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
  const auto press = simulation.pressLane(1, context);
  const auto first = simulation.releaseLane(1, context);
  const auto firstNotesExamined = simulation.lastSearchStats().notesExamined;
  const auto repress = simulation.pressLane(1, context);
  const auto second = simulation.releaseLane(1, context);
  const auto secondNotesExamined = simulation.lastSearchStats().notesExamined;

  require(press.noteId != gameplay::kInvalidNoteId &&
              repress.noteId == gameplay::kInvalidNoteId,
          "release boundary setup presses the valid note then re-presses its lane");
  require(first.noteId == gameplay::kInvalidNoteId &&
              second.noteId == gameplay::kInvalidNoteId,
          "notes at the practice end remain excluded from release selection");
  require(firstNotesExamined == 2 && secondNotesExamined == 0,
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

void testTransactionsOwnScoreGaugeAndPostStateReplay() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  chart.Meta.KeyMode = 5;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 260.0;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(1, new bms_parser::Note(42));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto press = simulation.pressLane(
      1, {.songTimeMicros = 1'000'000,
          .laneBeamTimeMicros = 2'000'000});

  require(press.hasJudge && simulation.scoreState().getScore() == 2,
          "accepted press commits EX score before return");
  require(simulation.scoreState().combo == 1,
          "accepted press commits combo before return");
  require(press.replayEvent.score == 2 && press.replayEvent.combo == 1 &&
              press.replayEvent.gauge ==
                  simulation.scoreState().currentGauge &&
              press.replayEvent.gaugeType ==
                  simulation.scoreState().gaugeType,
          "replay payload snapshots post-transaction state");
  require(simulation.replayEvents().size() == 1 &&
              simulation.replayEvents().front() == press.replayEvent,
          "simulation owns replay accumulation");

  const auto postJudge = simulation.snapshot();
  require(postJudge.judgeCounts[PGreat] == 1 && postJudge.score == 2 &&
              postJudge.combo == 1 && postJudge.maxCombo == 1 &&
              postJudge.gauge == simulation.scoreState().currentGauge &&
              postJudge.gaugeType == simulation.scoreState().gaugeType &&
              postJudge.clearTypeRank ==
                  simulation.scoreState().getClearTypeRank(),
          "attempt snapshot mirrors the complete committed score state");

  const auto emptyPress = simulation.pressLane(
      3, {.songTimeMicros = 1'100'000,
          .laneBeamTimeMicros = 2'100'000});
  const auto emptyRelease = simulation.releaseLane(
      3, {.songTimeMicros = 1'200'000,
          .laneBeamTimeMicros = 2'200'000});
  require(!emptyPress.hasJudge && !emptyRelease.hasJudge &&
              sameAttemptSnapshot(postJudge, simulation.snapshot()),
          "empty-lane press and release preserve post-judge attempt state");
  require(emptyPress.replayEvent.score == postJudge.score &&
              emptyPress.replayEvent.combo == postJudge.combo &&
              emptyPress.replayEvent.gauge == postJudge.gauge &&
              emptyPress.replayEvent.gaugeType == postJudge.gaugeType &&
              emptyRelease.replayEvent.score == postJudge.score &&
              emptyRelease.replayEvent.combo == postJudge.combo &&
              emptyRelease.replayEvent.gauge == postJudge.gauge &&
              emptyRelease.replayEvent.gaugeType == postJudge.gaugeType,
          "empty-lane replay payloads capture unchanged post-state");
  require(simulation.replayEvents().size() == 3 &&
              simulation.replayEvents()[1] == emptyPress.replayEvent &&
              simulation.replayEvents()[2] == emptyRelease.replayEvent,
          "empty-lane transactions append complete replay events in order");
}

void testBadAndPoorTransactionsMatchStandaloneScoreState() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 4;
  chart.Meta.KeyMode = 7;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 260.0;
  auto *measure = new bms_parser::Measure();
  auto *first = addTimeline(*measure, 1'000'000);
  first->SetNote(1, new bms_parser::Note(1));
  auto *bad = addTimeline(*measure, 2'000'000);
  bad->SetNote(2, new bms_parser::Note(2));
  addLongNote(*measure, 3'000'000, 3'500'000, 7,
              bms_parser::LongNoteType::ChargeNote);
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto metadata = definition.metadata();
  GameplayScoreState standalone({.totalNotes = metadata.totalNotes,
                                 .keyMode = metadata.keyMode,
                                 .gaugeTotal = metadata.gaugeTotal});
  standalone.configureBoundedGaugeHistory(4096);

  const auto commitBoth = [&](const gameplay::GameplayInputResult &result) {
    require(result.hasJudge, "combo parity transaction produces a judgement");
    standalone.commitJudge(result.judge);
    requireSameScoreState(simulation.scoreState(), standalone);
  };

  commitBoth(simulation.pressLane(1, {.songTimeMicros = 1'000'000}));
  const auto badPress =
      simulation.pressLane(2, {.songTimeMicros = 2'200'000});
  require(badPress.judge.judgement == Bad,
          "late normal-note press supplies the Bad combo break");
  commitBoth(badPress);
  commitBoth(simulation.pressLane(7, {.songTimeMicros = 3'000'000}));
  const auto poorRelease =
      simulation.releaseLane(7, {.songTimeMicros = 3'500'000}, false);
  require(poorRelease.judge.judgement == Poor,
          "non-backspin scratch release supplies the Poor combo break");
  commitBoth(poorRelease);

  require(simulation.scoreState().judgeCount.at(Bad) == 1 &&
              simulation.scoreState().judgeCount.at(Poor) == 1 &&
              simulation.scoreState().comboBreak == 2 &&
              simulation.scoreState().combo == 0,
          "Bad and Poor each commit a combo break");
  require(simulation.replayEvents().size() == 4 &&
              simulation.replayEvents().back() == poorRelease.replayEvent &&
              poorRelease.replayEvent.combo == 0 &&
              poorRelease.replayEvent.score ==
                  simulation.scoreState().getScore() &&
              poorRelease.replayEvent.gauge ==
                  simulation.scoreState().currentGauge,
          "combo-break replay snapshots the final post-transaction state");
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
  const auto headId = laneNotes[0];
  const auto tailId = laneNotes[1];
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});

  const auto press = simulation.pressLane(
      1, {.songTimeMicros = 1'000'000,
          .laneBeamTimeMicros = 3'000'000});

  require(press.noteId == gameplay::kInvalidNoteId &&
              press.soundNoteId == headId,
          "press near a long tail claims no note and reuses the pressable "
          "head keysound");
  require(simulation.lanePressed(1) &&
              !simulation.noteState(tailId).played &&
              !simulation.noteState(tailId).holding,
          "long-tail ineligibility preserves lane state without note mutation");
  require(!press.hasJudge && press.hasReplayEvent && press.hasLaneVisual &&
              press.replayEvent.noteId == gameplay::kInvalidNoteId,
          "tail-only press returns lane replay and visual intent without a judge");
}

void testLongTailCannotMaskLaterPressCandidateForAnyPriorityMode() {
  bool allPrioritiesSelectedNormal = true;
  for (const auto priority : {
           AppSettings::NotePriorityMode::Lowest,
           AppSettings::NotePriorityMode::Duration,
           AppSettings::NotePriorityMode::Combo,
           AppSettings::NotePriorityMode::Score}) {
    bms_parser::Chart chart;
    auto *measure = new bms_parser::Measure();
    addLongNote(*measure, 0, 990'000, 1,
                bms_parser::LongNoteType::LongNote);
    auto *normalTimeline = addTimeline(*measure, 1'100'000);
    normalTimeline->SetNote(1, new bms_parser::Note(19));
    chart.Measures.push_back(measure);

    const auto definition = gameplay::buildGameplayDefinition(chart, 0);
    gameplay::NoteId tailId = gameplay::kInvalidNoteId;
    gameplay::NoteId normalId = gameplay::kInvalidNoteId;
    for (const auto id : definition.laneNotes(1)) {
      const auto &note = definition.note(id);
      if (note.kind == gameplay::NoteKind::LongTail) {
        tailId = id;
      } else if (note.kind == gameplay::NoteKind::Normal) {
        normalId = id;
      }
    }

    gameplay::GameplaySimulation simulation(
        definition,
        {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
         .notePriorityMode = priority});
    const auto press = simulation.pressLane(
        1, {.songTimeMicros = 1'000'000,
            .laneBeamTimeMicros = 2'000'000});

    const bool selectedNormal =
        tailId != gameplay::kInvalidNoteId &&
        normalId != gameplay::kInvalidNoteId && press.noteId == normalId &&
        press.soundNoteId == normalId && press.hasJudge &&
        press.hasReplayEvent && press.replayEvent.noteId == normalId &&
        simulation.noteState(normalId).played &&
        !simulation.noteState(tailId).played && simulation.lanePressed(1);
    allPrioritiesSelectedNormal =
        allPrioritiesSelectedNormal && selectedNormal;
  }

  require(allPrioritiesSelectedNormal,
          "a long tail cannot mask a later playable normal under any priority");
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

int legacyManualKeysoundAt(long long inputMicros, bool markLastDead = false) {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *firstTimeline = addTimeline(*measure, 1'000'000);
  firstTimeline->SetNote(1, new bms_parser::Note(11));
  auto *lastTimeline = addTimeline(*measure, 2'000'000);
  auto *last = new bms_parser::Note(22);
  lastTimeline->SetNote(1, last);
  chart.Measures.push_back(measure);
  if (markLastDead) {
    last->IsPlayed = true;
    last->IsDead = true;
  }
  std::unordered_map<int, bool> lanes;
  RhythmLaneInputController controller(&chart, nullptr, lanes, Judge(1));
  const auto result = controller.pressLane(
      1, {.songTimeMicros = inputMicros,
          .laneBeamTimeMicros = inputMicros});
  return result.keySoundNote == nullptr ? bms_parser::Parser::NoWav
                                        : result.keySoundNote->Wav;
}

int simulationManualKeysoundAt(long long inputMicros,
                               bool expireLaneFirst = false) {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  addTimeline(*measure, 1'000'000)->SetNote(1, new bms_parser::Note(11));
  addTimeline(*measure, 2'000'000)->SetNote(1, new bms_parser::Note(22));
  addTimeline(*measure, 10'000'000)->SetNote(2, new bms_parser::Note(33));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  if (expireLaneFirst) {
    (void)simulation.advanceTo(3'000'000, 3'000'000);
    const auto laneNotes = definition.laneNotes(1);
    require(!laneNotes.empty() && simulation.noteState(laneNotes.back()).dead,
            "realtime dead-note fallback fixture expires the last lane note");
  }
  const auto result = simulation.pressLane(
      1, {.songTimeMicros = inputMicros,
          .laneBeamTimeMicros = inputMicros});
  return result.soundNoteId == gameplay::kInvalidNoteId
             ? bms_parser::Parser::NoWav
             : definition.keysoundSource(result.soundNoteId).wav;
}

void testManualKeysoundFallbackMatchesAcrossAuthorities() {
  for (const auto [inputMicros, expectedWav] : {
           std::pair{0LL, 11},
           std::pair{1'000'000LL, 11},
           std::pair{1'100'000LL, 11},
           std::pair{1'490'000LL, 22},
           std::pair{3'000'000LL, 22},
       }) {
    require(legacyManualKeysoundAt(inputMicros) == expectedWav &&
                simulationManualKeysoundAt(inputMicros) == expectedWav,
            "legacy and realtime select the same next-or-last keysound");
  }
  require(legacyManualKeysoundAt(3'000'000, true) == 22 &&
              simulationManualKeysoundAt(3'000'000, true) == 22,
          "the last pressed or dead note remains a fallback source");
}

void testInvisibleNotesChangeManualKeysoundWithoutJudgement() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  auto *measure = new bms_parser::Measure();
  addTimeline(*measure, 1'000'000)
      ->SetNote(1, new bms_parser::Note(11));
  auto *invisibleTimeline = addTimeline(*measure, 2'000'000);
  auto *invisible = new bms_parser::Note(77);
  invisibleTimeline->SetInvisibleNote(1, invisible);
  chart.Measures.push_back(measure);

  std::unordered_map<int, bool> lanes;
  RhythmLaneInputController legacy(&chart, nullptr, lanes, Judge(1));
  const auto legacyResult = legacy.pressLane(
      1, {.songTimeMicros = 2'000'000,
          .laneBeamTimeMicros = 2'000'000});
  require(legacyResult.note == nullptr && !legacyResult.hasJudge &&
              legacyResult.keySoundNote == invisible,
          "legacy lookup honors an invisible keysound without judging it");

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto keysounds = definition.laneKeysoundNotes(1);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto realtimeResult = simulation.pressLane(
      1, {.songTimeMicros = 2'000'000,
          .laneBeamTimeMicros = 2'000'000});
  require(definition.noteCount() == 1 &&
              definition.keysoundSourceCount() == 2 &&
              definition.laneNotes(1).size() == 1 && keysounds.size() == 2 &&
              realtimeResult.noteId == gameplay::kInvalidNoteId &&
              !realtimeResult.hasJudge &&
              realtimeResult.soundNoteId == keysounds.back() &&
              definition.keysoundSource(realtimeResult.soundNoteId).wav == 77,
          "realtime lookup compiles invisible keysounds outside judgement "
          "identities");
}

void testFallbackTieAndNoWavDoNotSkipSelection() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  addTimeline(*measure, 2'000'000)
      ->SetNote(1, new bms_parser::Note(41));
  addTimeline(*measure, 2'000'000)
      ->SetNote(2, new bms_parser::Note(bms_parser::Parser::NoWav));
  addTimeline(*measure, 3'000'000)
      ->SetNote(2, new bms_parser::Note(42));
  chart.Measures.push_back(measure);

  std::unordered_map<int, bool> lanes;
  RhythmLaneInputController legacy(&chart, nullptr, lanes, Judge(1));
  const auto oldResult = legacy.pressLane(
      2, 1, {.songTimeMicros = 1'000'000,
             .laneBeamTimeMicros = 1'000'000});
  require(oldResult.keySoundNote != nullptr &&
              oldResult.keySoundNote->Lane == 2 &&
              oldResult.keySoundNote->Wav == bms_parser::Parser::NoWav,
          "main-lane equal-time NoWav is selected without skipping");

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto newResult = simulation.pressLane(
      2, 1, {.songTimeMicros = 1'000'000,
             .laneBeamTimeMicros = 1'000'000});
  require(newResult.soundNoteId != gameplay::kInvalidNoteId &&
              definition.note(newResult.soundNoteId).lane == 2 &&
              definition.note(newResult.soundNoteId).wav ==
                  bms_parser::Parser::NoWav,
          "realtime keeps the same main-lane NoWav selection");
}

void testLongTailIsNotAManualPressKeysound() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *head = addLongNote(*measure, 1'000'000, 2'000'000, 1,
                           bms_parser::LongNoteType::ChargeNote);
  head->Wav = 51;
  head->Tail->Wav = 52;
  chart.Measures.push_back(measure);

  std::unordered_map<int, bool> lanes;
  RhythmLaneInputController legacy(&chart, nullptr, lanes, Judge(1));
  const auto oldResult = legacy.pressLane(
      1, {.songTimeMicros = 2'000'000,
          .laneBeamTimeMicros = 2'000'000});
  require(oldResult.note == nullptr && oldResult.keySoundNote == head,
          "legacy skips a judgeable long tail and falls back to its head");

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto newResult = simulation.pressLane(
      1, {.songTimeMicros = 2'000'000,
          .laneBeamTimeMicros = 2'000'000});
  require(newResult.noteId == gameplay::kInvalidNoteId &&
              newResult.soundNoteId != gameplay::kInvalidNoteId &&
              definition.note(newResult.soundNoteId).kind ==
                  gameplay::NoteKind::LongHead,
          "realtime press fallback excludes the long tail");
}

void testPreparationTransactionsOnlyChangeLaneReplayVisualAndSound() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  auto *measure = new bms_parser::Measure();
  addTimeline(*measure, 1'000'000)->SetNote(1, new bms_parser::Note(61));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto before = simulation.snapshot();
  const gameplay::GameplayInputContext preparation{
      .songTimeMicros = 900'000,
      .laneBeamTimeMicros = 90,
  };

  const auto preview =
      simulation.previewPreparationPressSoundNote(1, 1, preparation);
  const auto press =
      simulation.pressLaneForPreparation(1, 1, preparation);
  require(preview == press.soundNoteId &&
              press.soundNoteId != gameplay::kInvalidNoteId &&
              press.noteId == gameplay::kInvalidNoteId && !press.hasJudge &&
              press.hasLaneVisual &&
              press.laneVisual.action == gameplay::LaneVisualAction::Press &&
              press.hasReplayEvent && simulation.lanePressed(1),
          "preparation press publishes sound, lane visual, replay, and held "
          "state only");
  require(!simulation.noteState(0).played &&
              !simulation.noteState(0).dead &&
              sameAttemptSnapshot(before, simulation.snapshot()),
          "preparation press does not resolve gameplay state");

  const auto duplicate =
      simulation.pressLaneForPreparation(1, 1, preparation);
  require(duplicate.soundNoteId == gameplay::kInvalidNoteId &&
              !duplicate.hasLaneVisual && !duplicate.hasReplayEvent,
          "a held preparation lane rejects duplicate presses");

  const auto activeWhileHeld =
      simulation.pressLane(1, {.songTimeMicros = 1'000'000,
                               .laneBeamTimeMicros = 100});
  require(activeWhileHeld.noteId == gameplay::kInvalidNoteId &&
              !simulation.noteState(0).played,
          "a key held through activation cannot judge automatically");

  const auto release = simulation.releaseLaneForPreparation(
      1, {.songTimeMicros = 1'010'000, .laneBeamTimeMicros = 101});
  require(release.hasLaneVisual &&
              release.laneVisual.action ==
                  gameplay::LaneVisualAction::Release &&
              release.hasReplayEvent && !release.hasJudge &&
              !simulation.lanePressed(1),
          "preparation release clears the held lane without judgement");

  const auto activeAfterRelease =
      simulation.pressLane(1, {.songTimeMicros = 1'020'000,
                               .laneBeamTimeMicros = 102});
  require(activeAfterRelease.noteId != gameplay::kInvalidNoteId &&
              activeAfterRelease.hasJudge &&
              simulation.noteState(activeAfterRelease.noteId).played,
          "release and repress after activation follows ordinary judgement");
}

void testLegacyPreparationControllerCommitsSoundAndHeldStateOnly() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  auto *note = new bms_parser::Note(71);
  timeline->SetNote(1, note);
  chart.Measures.push_back(measure);
  std::unordered_map<int, bool> lanes;
  RhythmLaneInputController controller(&chart, nullptr, lanes, Judge(1));

  const RhythmLaneInputController::InputContext preparation{
      .songTimeMicros = 900'000,
      .laneBeamTimeMicros = 90,
  };
  const auto press =
      controller.pressLaneForPreparation(1, 1, preparation);
  require(press.note == nullptr && press.keySoundNote == note &&
              !press.hasJudge && press.hasReplayEvent && lanes.at(1) &&
              !note->IsPlayed && !note->IsDead,
          "legacy preparation press commits sound, replay, and held state "
          "without judgement");

  const auto release =
      controller.releaseLaneForPreparation(1, preparation);
  require(release.note == nullptr && release.keySoundNote == nullptr &&
              !release.hasJudge && release.hasReplayEvent && !lanes.at(1) &&
              !note->IsPlayed && !note->IsDead,
          "legacy preparation release clears held state without note "
          "mutation");
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

struct EmptyLaneTransactionSummary {
  PressSummary press;
  bool pressedAfterPress = false;
  PressSummary release;
  bool pressedAfterRelease = false;

  bool operator==(const EmptyLaneTransactionSummary &) const = default;
};

EmptyLaneTransactionSummary oldEmptyLaneTransactions() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 5;
  std::unordered_map<int, bool> lanes;
  RhythmLaneInputController controller(&chart, nullptr, lanes, Judge(1));
  const auto press = controller.pressLane(
      3, {.songTimeMicros = 1'000'000,
          .laneBeamTimeMicros = 2'000'000});
  const bool pressedAfterPress = lanes.at(3);
  const auto release = controller.releaseLane(
      3, {.songTimeMicros = 1'100'000,
          .laneBeamTimeMicros = 2'100'000});
  return {
      .press = {
          .selected = oldNoteIdentity(press.note),
          .sound = oldNoteIdentity(press.keySoundNote),
          .judge = {.present = press.hasJudge,
                    .judgement = press.judge.judgement,
                    .diffMicros = press.judge.Diff},
          .replay = oldReplaySummary(press.hasReplayEvent, press.replayEvent),
      },
      .pressedAfterPress = pressedAfterPress,
      .release = {
          .selected = oldNoteIdentity(release.note),
          .sound = oldNoteIdentity(release.keySoundNote),
          .judge = {.present = release.hasJudge,
                    .judgement = release.judge.judgement,
                    .diffMicros = release.judge.Diff},
          .replay =
              oldReplaySummary(release.hasReplayEvent, release.replayEvent),
      },
      .pressedAfterRelease = lanes.at(3),
  };
}

EmptyLaneTransactionSummary newEmptyLaneTransactions() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 5;
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  const auto press = simulation.pressLane(
      3, {.songTimeMicros = 1'000'000,
          .laneBeamTimeMicros = 2'000'000});
  const bool pressedAfterPress = simulation.lanePressed(3);
  const auto release = simulation.releaseLane(
      3, {.songTimeMicros = 1'100'000,
          .laneBeamTimeMicros = 2'100'000});
  return {
      .press = {
          .selected = newNoteIdentity(definition, press.noteId),
          .sound = newNoteIdentity(definition, press.soundNoteId),
          .judge = {.present = press.hasJudge,
                    .judgement = press.judge.judgement,
                    .diffMicros = press.judge.Diff},
          .replay = newReplaySummary(press.hasReplayEvent, press.replayEvent,
                                     definition),
      },
      .pressedAfterPress = pressedAfterPress,
      .release = {
          .selected = newNoteIdentity(definition, release.noteId),
          .sound = newNoteIdentity(definition, release.soundNoteId),
          .judge = {.present = release.hasJudge,
                    .judgement = release.judge.judgement,
                    .diffMicros = release.judge.Diff},
          .replay = newReplaySummary(release.hasReplayEvent,
                                     release.replayEvent, definition),
      },
      .pressedAfterRelease = simulation.lanePressed(3),
  };
}

void testEmptyValidLaneMatchesCurrentController() {
  require(oldEmptyLaneTransactions() == newEmptyLaneTransactions(),
          "empty valid lane transactions match the current controller");
}

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

PressSummary oldTwoLaneEqualTimePress(
    AppSettings::NotePriorityMode priority) {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(1, new bms_parser::Note(1));
  timeline->SetNote(2, new bms_parser::Note(2));
  chart.Measures.push_back(measure);
  std::unordered_map<int, bool> lanes{{1, false}, {2, false}};
  RhythmLaneInputController controller(&chart, nullptr, lanes, Judge(1));
  const auto result = controller.pressLane(
      2, 1, {.songTimeMicros = 1'100'000,
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

PressSummary newTwoLaneEqualTimePress(
    AppSettings::NotePriorityMode priority) {
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
       .notePriorityMode = priority});
  const auto result = simulation.pressLane(
      2, 1, {.songTimeMicros = 1'100'000,
             .laneBeamTimeMicros = 2'000'000});
  return {
      .selected = newNoteIdentity(definition, result.noteId),
      .sound = newNoteIdentity(definition, result.soundNoteId),
      .judge = {.present = result.hasJudge,
                .judgement = result.judge.judgement,
                .diffMicros = result.judge.Diff},
      .replay = newReplaySummary(result.hasReplayEvent, result.replayEvent,
                                 definition),
  };
}

void testTwoLaneEqualTimePressMatchesCurrentController() {
  for (const auto priority : {
           AppSettings::NotePriorityMode::Lowest,
           AppSettings::NotePriorityMode::Duration,
           AppSettings::NotePriorityMode::Combo,
           AppSettings::NotePriorityMode::Score}) {
    const auto oldResult = oldTwoLaneEqualTimePress(priority);
    const auto newResult = newTwoLaneEqualTimePress(priority);
    require(oldResult == newResult,
            "equal-time two-lane press matches current controller outcome");
  }
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

gameplay::CompiledGameplayJudge lr2Judge(int rank = 2) {
  return gameplay::CompiledGameplayJudge::from(
      gameplay::compileGameplayJudgeRules(GameplayRuleset::LR2, rank));
}

void testSharedNoteRoleClassification() {
  using gameplay::NoteJudgeRole;
  using gameplay::NoteKind;
  const std::array immutableCases{
      std::pair{gameplay::NoteDefinition{.kind = NoteKind::Normal},
                NoteJudgeRole::Normal},
      std::pair{gameplay::NoteDefinition{.kind = NoteKind::Normal,
                                         .scratchLane = true},
                NoteJudgeRole::Scratch},
      std::pair{gameplay::NoteDefinition{.kind = NoteKind::LongHead},
                NoteJudgeRole::LongNoteHead},
      std::pair{gameplay::NoteDefinition{.kind = NoteKind::LongHead,
                                         .scratchLane = true},
                NoteJudgeRole::LongScratchHead},
      std::pair{gameplay::NoteDefinition{.kind = NoteKind::LongTail},
                NoteJudgeRole::LongNoteTail},
      std::pair{gameplay::NoteDefinition{.kind = NoteKind::LongTail,
                                         .scratchLane = true},
                NoteJudgeRole::LongScratchTail},
  };
  for (const auto &[note, expected] : immutableCases) {
    require(gameplay::judgeRoleFor(note) == expected,
            "immutable notes use the shared judge-role classifier");
  }

  bms_parser::Chart chart;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  auto *normalTimeline = addTimeline(*measure, 100);
  auto *normal = new bms_parser::Note(1);
  normalTimeline->SetNote(1, normal);
  auto *scratchTimeline = addTimeline(*measure, 200);
  auto *scratch = new bms_parser::Note(2);
  scratchTimeline->SetNote(7, scratch);
  auto *normalLong = addLongNote(*measure, 300, 400, 1,
                                 bms_parser::LongNoteType::LongNote);
  auto *scratchLong = addLongNote(*measure, 500, 600, 7,
                                  bms_parser::LongNoteType::ChargeNote);
  chart.Measures.push_back(measure);
  require(gameplay::judgeRoleFor(normal, chart.Meta, 0) ==
                  NoteJudgeRole::Normal &&
              gameplay::judgeRoleFor(scratch, chart.Meta, 0) ==
                  NoteJudgeRole::Scratch &&
              gameplay::judgeRoleFor(normalLong, chart.Meta, 0) ==
                  NoteJudgeRole::LongNoteHead &&
              gameplay::judgeRoleFor(normalLong->Tail, chart.Meta, 0) ==
                  NoteJudgeRole::LongNoteTail &&
              gameplay::judgeRoleFor(scratchLong, chart.Meta, 0) ==
                  NoteJudgeRole::LongScratchHead &&
              gameplay::judgeRoleFor(scratchLong->Tail, chart.Meta, 0) ==
                  NoteJudgeRole::LongScratchTail,
          "parser notes use the same six judge roles");
}

void testLr2CandidateFilterGoldenClusters() {
  using gameplay::JudgeCandidateDescriptor;
  std::array<std::size_t, 8> multiBad{};

  const std::array selectedGood{
      JudgeCandidateDescriptor{.sourceIndex = 10,
                               .timingMicros = 800,
                               .judge = JudgeResult(Bad, 200)},
      JudgeCandidateDescriptor{.sourceIndex = 11,
                               .timingMicros = 950,
                               .judge = JudgeResult(Good, 50)},
      JudgeCandidateDescriptor{.sourceIndex = 12,
                               .timingMicros = 1150,
                               .judge = JudgeResult(Bad, -150)},
  };
  auto resolution = gameplay::resolveLr2Candidates(selectedGood, multiBad);
  require(resolution.selectedSourceIndex == 11 &&
              resolution.multiBadCount == 1 && multiBad[0] == 10,
          "LR2 Combo selection keeps preceding late BAD before selected GOOD");

  const std::array selectedBad{
      JudgeCandidateDescriptor{.sourceIndex = 20,
                               .timingMicros = 820,
                               .judge = JudgeResult(Bad, 180)},
      JudgeCandidateDescriptor{.sourceIndex = 21,
                               .timingMicros = 850,
                               .judge = JudgeResult(Bad, 150)},
      JudgeCandidateDescriptor{.sourceIndex = 22,
                               .timingMicros = 1150,
                               .judge = JudgeResult(Bad, -150)},
  };
  resolution = gameplay::resolveLr2Candidates(selectedBad, multiBad);
  require(resolution.selectedSourceIndex == 21 &&
              resolution.multiBadCount == 2 && multiBad[0] == 20 &&
              multiBad[1] == 22,
          "a selected LR2 BAD keeps both sorted surrounding multi-BAD notes");

  const std::array precedingLongNote{
      JudgeCandidateDescriptor{.sourceIndex = 30,
                               .timingMicros = 800,
                               .longNoteHead = true,
                               .judge = JudgeResult(Bad, 200)},
      JudgeCandidateDescriptor{.sourceIndex = 31,
                               .timingMicros = 950,
                               .judge = JudgeResult(Good, 50)},
  };
  resolution = gameplay::resolveLr2Candidates(precedingLongNote, multiBad);
  require(resolution.selectedSourceIndex == 31 &&
              resolution.multiBadCount == 0,
          "leading LR2 long-note heads are removed from multi-BAD");
}

void testLr2RepeatedKpoorAndStrictAutomaticPoor() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 5;
  chart.Meta.TotalNotes = 1;
  auto *measure = new bms_parser::Measure();
  addTimeline(*measure, 1'000'000)->SetNote(1, new bms_parser::Note(1));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(definition, {.judge = lr2Judge()});

  const auto first =
      simulation.pressLane(1, {.songTimeMicros = 100'000});
  require(first.transactions.size() == 1 && first.hasJudge &&
              first.judge.judgement == Kpoor &&
              !simulation.noteState(0).played &&
              !simulation.noteState(0).dead && simulation.snapshot().combo == 0 &&
              simulation.snapshot().comboBreak == 0,
          "early LR2 press emits KPoor without consuming the note or combo");
  simulation.releaseLane(1, {.songTimeMicros = 100'001});
  const auto second =
      simulation.pressLane(1, {.songTimeMicros = 500'000});
  require(second.transactions.size() == 1 && second.hasJudge &&
              second.judge.judgement == Kpoor &&
              simulation.snapshot().judgeCounts[Kpoor] == 2 &&
              !simulation.noteState(0).played,
          "the same future note can emit repeated LR2 KPoor");
  simulation.releaseLane(1, {.songTimeMicros = 500'001});

  const auto atBoundary = simulation.advanceTo(1'200'000, 1'200'000);
  require(atBoundary.transactions.empty() && !simulation.noteState(0).played,
          "LR2 note remains playable at the inclusive +200 ms boundary");
  const auto afterBoundary = simulation.advanceTo(1'200'001, 1'200'001);
  require(afterBoundary.transactions.size() == 1 &&
              afterBoundary.transactions.front().judge.judgement == Poor &&
              simulation.noteState(0).played &&
              simulation.snapshot().judgeCounts[Poor] == 1,
          "LR2 automatic POOR occurs exactly at +200001 microseconds");
}

void testLr2MultiBadBatchAndFixedSelection() {
  const auto run = [](AppSettings::NotePriorityMode mode) {
    bms_parser::Chart chart;
    chart.Meta.KeyMode = 5;
    chart.Meta.TotalNotes = 3;
    auto *measure = new bms_parser::Measure();
    addTimeline(*measure, 800'000)->SetNote(1, new bms_parser::Note(1));
    addTimeline(*measure, 950'000)->SetNote(2, new bms_parser::Note(2));
    addTimeline(*measure, 1'150'000)->SetNote(1, new bms_parser::Note(3));
    chart.Measures.push_back(measure);
    const auto definition = gameplay::buildGameplayDefinition(chart, 0);
    gameplay::GameplaySimulation simulation(
        definition, {.judge = lr2Judge(), .notePriorityMode = mode});
    const auto batch = simulation.pressLane(
        1, 2, {.songTimeMicros = 1'000'000,
               .laneBeamTimeMicros = 7});
    require(batch.transactions.size() == 2,
            "LR2 press emits multi-BAD followed by selected transaction");
    const auto &multiBad = batch.transactions[0];
    const auto &selected = batch.transactions[1];
    require(multiBad.noteId == 0 && multiBad.hasJudge &&
                multiBad.judge.judgement == Bad &&
                multiBad.soundNoteId == gameplay::kInvalidNoteId &&
                !multiBad.hasLaneVisual && selected.noteId == 1 &&
                selected.judge.judgement == Good &&
                selected.soundNoteId == 1 && selected.hasLaneVisual &&
                batch.noteId == selected.noteId &&
                simulation.noteState(0).played &&
                simulation.noteState(1).played &&
                !simulation.noteState(2).played,
            "LR2 batch ordering and selected-only sound/visual are stable");
    return std::array{batch.transactions[0].noteId,
                      batch.transactions[1].noteId};
  };

  const auto expected = run(AppSettings::NotePriorityMode::Lowest);
  for (const auto mode : {AppSettings::NotePriorityMode::Combo,
                          AppSettings::NotePriorityMode::Duration,
                          AppSettings::NotePriorityMode::Score}) {
    require(run(mode) == expected,
            "application note-priority setting cannot alter LR2 selection");
  }
}

void testBeatorajaStillEmitsOnePrioritySelectedTransaction() {
  const auto run = [](AppSettings::NotePriorityMode mode) {
    bms_parser::Chart chart;
    chart.Meta.KeyMode = 5;
    chart.Meta.TotalNotes = 2;
    auto *measure = new bms_parser::Measure();
    addTimeline(*measure, 800'000)->SetNote(1, new bms_parser::Note(1));
    addTimeline(*measure, 950'000)->SetNote(1, new bms_parser::Note(2));
    chart.Measures.push_back(measure);
    const auto definition = gameplay::buildGameplayDefinition(chart, 0);
    gameplay::GameplaySimulation simulation(
        definition,
        {.judge = gameplay::CompiledGameplayJudge::from(Judge(2)),
         .notePriorityMode = mode});
    return simulation.pressLane(1, {.songTimeMicros = 1'000'000});
  };

  const auto lowest = run(AppSettings::NotePriorityMode::Lowest);
  require(lowest.transactions.size() == 1 && lowest.noteId == 0,
          "Beatoraja Lowest keeps the earliest candidate and one transaction");
  const auto combo = run(AppSettings::NotePriorityMode::Combo);
  require(combo.transactions.size() == 1 && combo.noteId == 1,
          "Beatoraja Combo keeps its existing priority and one transaction");
}

void testLegacyControllerUsesSharedLr2BatchResolution() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 5;
  chart.Meta.TotalNotes = 3;
  auto *measure = new bms_parser::Measure();
  auto *multiBad = new bms_parser::Note(1);
  auto *selected = new bms_parser::Note(2);
  auto *future = new bms_parser::Note(3);
  addTimeline(*measure, 800'000)->SetNote(1, multiBad);
  addTimeline(*measure, 950'000)->SetNote(2, selected);
  addTimeline(*measure, 1'150'000)->SetNote(1, future);
  chart.Measures.push_back(measure);
  std::unordered_map<int, bool> lanes;
  RhythmLaneInputController controller(&chart, nullptr, lanes, lr2Judge());
  const auto batch = controller.pressLane(
      1, 2, {.songTimeMicros = 1'000'000,
             .laneBeamTimeMicros = 1'000'000,
             .notePriorityMode = AppSettings::NotePriorityMode::Score});
  require(batch.transactions.size() == 2 &&
              batch.transactions[0].note == multiBad &&
              batch.transactions[0].judge.judgement == Bad &&
              batch.transactions[0].keySoundNote == nullptr &&
              batch.transactions[1].note == selected &&
              batch.transactions[1].judge.judgement == Good &&
              batch.transactions[1].keySoundNote == selected &&
              batch.note == selected && multiBad->IsPlayed &&
              selected->IsPlayed && !future->IsPlayed,
          "legacy controller consumes the same shared LR2 multi-BAD batch");
}
} // namespace

int main() {
  testCompiledJudgePreservesResolvedWindows();
  testManualKeysoundSelectionUsesFutureThenLastWithMainTies();
  testDefinitionKeysoundIndexExcludesTailsAndLandmines();
  testDefinitionUsesStableIdsAndLaneIndices();
  testEmptyValidLaneCommitsPressAndReleaseIntents();
  testCandidateSelectionIsLaneIndexed();
  testRejectedTransactionsResetLatestSearchStats();
  testCompensationAndPriorityMatchCurrentRules();
  testPracticeRangeIsHalfOpenBeforeLaneMutation();
  testEqualTimeLowestKeepsMainLanePrecedence();
  testEqualTimeFollowsLatePriorityModes();
  testReleaseSearchStopsAtPracticeEnd();
  testPressCommitsStateAndSoundTogether();
  testTransactionsOwnScoreGaugeAndPostStateReplay();
  testBadAndPoorTransactionsMatchStandaloneScoreState();
  testClassicLongHeadDefersJudgeButStillCommitsSoundAndHolding();
  testPressDoesNotClaimLongTail();
  testLongTailCannotMaskLaterPressCandidateForAnyPriorityMode();
  testClassicReleaseCommitsOneJudgeAndNoSound();
  testChargeScratchRequiresBackspinRelease();
  testParitySummaryDetectsPerturbedIdentityAndPayload();
  testEmptyValidLaneMatchesCurrentController();
  testTwoLaneEqualTimePressMatchesCurrentController();
  testManualKeysoundFallbackMatchesAcrossAuthorities();
  testInvisibleNotesChangeManualKeysoundWithoutJudgement();
  testFallbackTieAndNoWavDoNotSkipSelection();
  testLongTailIsNotAManualPressKeysound();
  testPreparationTransactionsOnlyChangeLaneReplayVisualAndSound();
  testLegacyPreparationControllerCommitsSoundAndHeldStateOnly();
  testCurrentPressParityMatrix();
  testCurrentReleaseParityMatrix();
  testSharedNoteRoleClassification();
  testLr2CandidateFilterGoldenClusters();
  testLr2RepeatedKpoorAndStrictAutomaticPoor();
  testLr2MultiBadBatchAndFixedSelection();
  testBeatorajaStillEmitsOnePrioritySelectedTransaction();
  testLegacyControllerUsesSharedLr2BatchResolution();
  return 0;
}
