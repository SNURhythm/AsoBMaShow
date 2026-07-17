#include "scene/play/GameplayDefinition.h"
#include "scene/play/GameplayScoreState.h"
#include "scene/play/GameplaySimulation.h"
#include "scene/play/Judge.h"

#include "bms_parser.hpp"

#include <algorithm>
#include <cstdint>
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

void testDefinitionCompilesAutomaticMetadata() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 2;
  chart.Meta.KeyMode = 7;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 260.0;
  auto *measure = new bms_parser::Measure();
  auto *late = addTimeline(*measure, 2'000'000);
  late->SetNote(1, new bms_parser::Note(20));
  auto *early = addTimeline(*measure, 1'000'000);
  early->SetNote(2, new bms_parser::Note(10));
  early->SetLandmineNote(3, new bms_parser::LandmineNote(3.5F));
  addTimeline(*measure, 3'000'000);
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto metadata = definition.metadata();
  require(metadata.totalNotes == 2 && metadata.keyMode == 7 &&
              metadata.gaugeTotal == 260.0,
          "chart gauge metadata is copied");

  const auto chronological = definition.chronologicalNotes();
  require(chronological.size() == 3,
          "all note identities are scheduled");
  require(definition.note(chronological[0]).timingMicros == 1'000'000 &&
              definition.note(chronological[1]).timingMicros == 1'000'000 &&
              definition.note(chronological[2]).timingMicros == 2'000'000,
          "automatic identities are chronological and stable");
  require(chronological[0] < chronological[1],
          "equal-time automatic identities use NoteId order");

  std::vector<int> occurrences(definition.noteCount());
  int landmineCount = 0;
  for (const gameplay::NoteId id : chronological) {
    require(id < occurrences.size(), "automatic identity is in range");
    ++occurrences[id];
    landmineCount +=
        definition.note(id).kind == gameplay::NoteKind::Landmine ? 1 : 0;
  }
  require(std::ranges::all_of(occurrences,
                              [](int count) { return count == 1; }) &&
              landmineCount == 1,
          "every runtime identity, including mines, appears exactly once");
  require(metadata.finalNoteTimeMicros == 2'000'000,
          "final note timing follows the chronological note index");
  require(metadata.finalTimelineTimeMicros == 3'000'000,
          "empty final timelines remain part of completion timing");
}

void testDefinitionUsesDefaultGaugeTotalWhenChartOmitsTotal() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 432;
  chart.Meta.KeyMode = 24;
  chart.Meta.HasTotal = false;

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  require(definition.metadata().gaugeTotal ==
              beatorajaDefaultGaugeTotal(chart.Meta.KeyMode,
                                         chart.Meta.TotalNotes),
          "missing chart total uses the shared beatoraja gauge rule");
}

void testDefinitionCompilesChronologicalHellChargeHeads() {
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  addLongNote(*measure, 3'000'000, 4'000'000, 1,
              bms_parser::LongNoteType::HellChargeNote);
  addLongNote(*measure, 1'000'000, 2'000'000, 2,
              bms_parser::LongNoteType::HellChargeNote);
  addLongNote(*measure, 500'000, 2'500'000, 3,
              bms_parser::LongNoteType::ChargeNote);
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto heads = definition.hellChargeHeads();
  require(heads.size() == 2,
          "only Hell Charge heads are included in the hot-path index");
  require(definition.note(heads[0]).timingMicros == 1'000'000 &&
              definition.note(heads[1]).timingMicros == 3'000'000,
          "Hell Charge heads follow global chronological order");
  for (const gameplay::NoteId id : heads) {
    const auto &note = definition.note(id);
    require(note.kind == gameplay::NoteKind::LongHead &&
                note.longNoteRule == gameplay::LongNoteRule::HellCharge,
            "Hell Charge index contains resolved heads only");
  }
}

void testAttemptInitializesConfiguredAndCarriedState() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 10;
  chart.Meta.KeyMode = 7;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 260.0;
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);

  gameplay::GameplaySimulation configured(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .attempt = {.initialGaugeType = GaugeType::Hard,
                   .gaugeAutoShift = GaugeAutoShiftMode::None,
                   .gaugeProfile = GaugeProfile::Standard,
                   .startingGaugePercent = 37,
                   .carriedCombo = 4,
                   .carriedMaxCombo = 6,
                   .assistClearMark = true,
                   .replayCapacity = 8,
                   .automaticResultCapacity = 8}});
  const auto configuredSnapshot = configured.snapshot();
  require(configured.scoreState().gaugeType == GaugeType::Hard &&
              configured.scoreState().currentGauge == 37.0F &&
              configuredSnapshot.gauge == 37.0F &&
              configuredSnapshot.gaugeType == GaugeType::Hard,
          "attempt applies configured gauge and starting percent");
  require(configuredSnapshot.combo == 4 &&
              configuredSnapshot.maxCombo == 6 &&
              configuredSnapshot.clearTypeRank ==
                  kClearTypeAssistedEasyClearRank,
          "attempt restores carried combo and assist-clear state");

  GameplayScoreState carried({.totalNotes = 10,
                              .keyMode = 7,
                              .gaugeTotal = 260.0});
  carried.configureGauge(GaugeType::Easy, GaugeAutoShiftMode::BestClear,
                         GaugeProfile::Standard, GaugeType::Easy);
  carried.setStartingGaugePercent(64);
  carried.applyGaugeJudgement(Bad);
  const auto carriedGauge = carried.gaugeSnapshot();
  gameplay::GameplaySimulation restored(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .attempt = {.initialGaugeType = GaugeType::ExHard,
                   .gaugeAutoShift = GaugeAutoShiftMode::None,
                   .gaugeProfile = GaugeProfile::CourseDefault,
                   .startingGaugePercent = 12,
                   .carriedGauge = carriedGauge,
                   .carriedCombo = 7,
                   .carriedMaxCombo = 11}});
  const auto restoredGauge = restored.scoreState().gaugeSnapshot();
  require(restoredGauge.gaugeType == carriedGauge.gaugeType &&
              restoredGauge.selectedGaugeType ==
                  carriedGauge.selectedGaugeType &&
              restoredGauge.gaugeAutoShiftLowerBound ==
                  carriedGauge.gaugeAutoShiftLowerBound &&
              restoredGauge.gaugeProfile == GaugeProfile::Course7Keys &&
              restoredGauge.gaugeAutoShift == carriedGauge.gaugeAutoShift &&
              restoredGauge.currentGauge == carriedGauge.currentGauge &&
              restoredGauge.gaugeValues == carriedGauge.gaugeValues &&
              restoredGauge.gaugeSurvivalFailed ==
                  carriedGauge.gaugeSurvivalFailed,
          "carried gauge values restore under the configured attempt profile");
  require(restored.snapshot().combo == 7 &&
              restored.snapshot().maxCombo == 11,
          "carried combo fields survive gauge restoration");
}

void testReplayAndGaugeHistoryCapacityLatchWithoutGrowth() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 2;
  chart.Meta.KeyMode = 5;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 260.0;
  auto *measure = new bms_parser::Measure();
  auto *first = addTimeline(*measure, 1'000'000);
  first->SetNote(1, new bms_parser::Note(1));
  auto *second = addTimeline(*measure, 2'000'000);
  second->SetNote(2, new bms_parser::Note(2));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);

  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .attempt = {.replayCapacity = 1, .automaticResultCapacity = 1}});
  const auto firstPress =
      simulation.pressLane(1, {.songTimeMicros = 1'000'000});
  require(firstPress.hasReplayEvent && simulation.replayEvents().size() == 1,
          "first replay fills the configured fixed storage");
  const auto *const replayStorage = simulation.replayEvents().data();
  const auto storedFirst = simulation.replayEvents().front();

  const auto secondPress =
      simulation.pressLane(2, {.songTimeMicros = 2'000'000});
  require(secondPress.hasJudge && secondPress.hasReplayEvent &&
              secondPress.replayEvent.score == 4 &&
              secondPress.replayEvent.combo == 2,
          "overflowing transaction still returns its committed post-state");
  require(simulation.replayOverflowed() &&
              simulation.replayEvents().size() == 1 &&
              simulation.replayEvents().data() == replayStorage &&
              simulation.replayEvents().front() == storedFirst,
          "replay capacity latches without growth or stored-event mutation");
  require(simulation.scoreState().gaugeHistory.size() == 1 &&
              simulation.scoreState().gaugeHistoryOverflowed(),
          "bounded gauge history also latches instead of allocating");
}

void testNormalLatePoorUsesStrictDeadlineAndIsIdempotent() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  auto *measure = new bms_parser::Measure();
  constexpr std::int64_t timingMicros = 1'000'000;
  auto *timeline = addTimeline(*measure, timingMicros);
  timeline->SetNote(1, new bms_parser::Note(1));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto compiledJudge = gameplay::CompiledGameplayJudge::from(Judge(1));
  const std::int64_t lateEdge =
      timingMicros + compiledJudge.latePoorTimingMicros();
  gameplay::GameplaySimulation simulation(definition, {.judge = compiledJudge});
  const gameplay::NoteId noteId = definition.chronologicalNotes().front();

  const auto atEdge = simulation.advanceTo(lateEdge, 7'000'000);
  require(atEdge.transactions.empty() && atEdge.advancedToMicros == lateEdge &&
              !simulation.noteState(noteId).played &&
              !simulation.noteState(noteId).dead &&
              simulation.snapshot().judgeCounts[Poor] == 0,
          "normal note remains untouched at the inclusive late Poor edge");

  const std::int64_t deadline = lateEdge + 1;
  const auto late = simulation.advanceTo(deadline, 7'000'001);
  require(late.transactions.size() == 1 && late.advancedToMicros == deadline,
          "strict late deadline emits exactly one transaction");
  const auto &miss = late.transactions.front();
  require(miss.noteId == noteId && miss.hasJudge &&
              miss.judge.judgement == Poor &&
              miss.judge.Diff == compiledJudge.latePoorTimingMicros() + 1 &&
              miss.hasReplayEvent &&
              miss.replayEvent.action == gameplay::GameplayReplayAction::Miss,
          "late normal note commits a Poor miss with the strict diff");
  require(simulation.noteState(noteId).played &&
              simulation.noteState(noteId).dead &&
              simulation.noteState(noteId).playedTimeMicros == deadline &&
              simulation.snapshot().judgeCounts[Poor] == 1 &&
              simulation.replayEvents().size() == 1,
          "late normal note commits state, score, and replay once");

  const auto repeated = simulation.advanceTo(deadline, 7'000'002);
  require(repeated.transactions.empty() &&
              repeated.advancedToMicros == deadline &&
              simulation.snapshot().judgeCounts[Poor] == 1 &&
              simulation.replayEvents().size() == 1,
          "repeating the same deadline is idempotent");
}

void testLandmineDetonatesOrExpiresFromPriorLaneState() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 5;
  auto *measure = new bms_parser::Measure();
  constexpr std::int64_t timingMicros = 1'000'000;
  auto *timeline = addTimeline(*measure, timingMicros);
  timeline->SetLandmineNote(1, new bms_parser::LandmineNote(3.5F));
  timeline->SetLandmineNote(2, new bms_parser::LandmineNote(7.0F));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::NoteId detonatedId = gameplay::kInvalidNoteId;
  gameplay::NoteId expiredId = gameplay::kInvalidNoteId;
  for (const gameplay::NoteId id : definition.chronologicalNotes()) {
    const auto &note = definition.note(id);
    if (note.lane == 1) {
      detonatedId = id;
    } else if (note.lane == 2) {
      expiredId = id;
    }
  }
  gameplay::GameplaySimulation simulation(
      definition, {.judge = gameplay::CompiledGameplayJudge::from(Judge(1))});
  simulation.pressLane(1, {.songTimeMicros = 0});
  const float gaugeBefore = simulation.snapshot().gauge;

  const auto advanced = simulation.advanceTo(timingMicros, 8'000'000);
  require(advanced.transactions.size() == 1 &&
              advanced.transactions.front().noteId == detonatedId &&
              advanced.transactions.front().hasReplayEvent &&
              advanced.transactions.front().replayEvent.action ==
                  gameplay::GameplayReplayAction::Mine,
          "pressed-lane mine emits one Mine transaction");
  require(simulation.noteState(detonatedId).played &&
              simulation.noteState(detonatedId).dead &&
              simulation.noteState(detonatedId).playedTimeMicros ==
                  timingMicros &&
              simulation.snapshot().gauge == gaugeBefore - 3.5F,
          "detonated mine commits state and exact negative damage");
  require(!simulation.noteState(expiredId).played &&
              simulation.noteState(expiredId).dead &&
              simulation.noteState(expiredId).playedTimeMicros == timingMicros,
          "unpressed-lane mine expires without becoming played");
  require(std::ranges::count(simulation.replayEvents(),
                             gameplay::GameplayReplayAction::Mine,
                             &gameplay::GameplayReplayEvent::action) == 1,
          "only the detonated mine records replay and gauge mutation");
}

void testSameTimeAutomaticWorkPrecedesPressAndRelease() {
  bms_parser::Chart chart;
  chart.Meta.KeyMode = 5;
  auto *measure = new bms_parser::Measure();
  constexpr std::int64_t timingMicros = 1'000'000;
  auto *timeline = addTimeline(*measure, timingMicros);
  timeline->SetLandmineNote(1, new bms_parser::LandmineNote(4.0F));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const gameplay::NoteId mineId = definition.chronologicalNotes().front();
  const gameplay::GameplaySimulationConfig config{
      .judge = gameplay::CompiledGameplayJudge::from(Judge(1))};

  gameplay::GameplaySimulation pressedAtDeadline(definition, config);
  const float pressGaugeBefore = pressedAtDeadline.snapshot().gauge;
  const auto press = pressedAtDeadline.applyPressAt(
      1, 1, {.songTimeMicros = timingMicros, .laneBeamTimeMicros = 9'000'000});
  require(!pressedAtDeadline.noteState(mineId).played &&
              pressedAtDeadline.noteState(mineId).dead &&
              pressedAtDeadline.snapshot().gauge == pressGaugeBefore &&
              pressedAtDeadline.automaticResults().empty() &&
              press.hasReplayEvent && pressedAtDeadline.lanePressed(1),
          "same-time press expires the mine using the prior unpressed lane");

  gameplay::GameplaySimulation releasedAtDeadline(definition, config);
  releasedAtDeadline.pressLane(1, {.songTimeMicros = 0});
  const float releaseGaugeBefore = releasedAtDeadline.snapshot().gauge;
  const auto release = releasedAtDeadline.applyReleaseAt(
      1, {.songTimeMicros = timingMicros, .laneBeamTimeMicros = 9'100'000});
  require(releasedAtDeadline.noteState(mineId).played &&
              releasedAtDeadline.noteState(mineId).dead &&
              releasedAtDeadline.snapshot().gauge ==
                  releaseGaugeBefore - 4.0F &&
              releasedAtDeadline.automaticResults().size() == 1 &&
              release.hasReplayEvent &&
              release.replayEvent.action ==
                  gameplay::GameplayReplayAction::Release &&
              !releasedAtDeadline.lanePressed(1),
          "same-time release detonates the mine before clearing the lane");
}

void testEqualDeadlineUsesAtTimingPhaseBeforeLatePoor() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  chart.Meta.KeyMode = 5;
  const auto compiledJudge = gameplay::CompiledGameplayJudge::from(Judge(1));
  constexpr std::int64_t sharedDeadline = 2'000'000;
  const std::int64_t normalTiming =
      sharedDeadline - compiledJudge.latePoorTimingMicros() - 1;
  auto *measure = new bms_parser::Measure();
  auto *normalTimeline = addTimeline(*measure, normalTiming);
  normalTimeline->SetNote(1, new bms_parser::Note(1));
  auto *mineTimeline = addTimeline(*measure, sharedDeadline);
  mineTimeline->SetLandmineNote(1, new bms_parser::LandmineNote(2.0F));
  mineTimeline->SetLandmineNote(2, new bms_parser::LandmineNote(1.0F));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto chronological = definition.chronologicalNotes();
  const gameplay::NoteId normalId = chronological[0];
  const gameplay::NoteId firstMineId = chronological[1];
  const gameplay::NoteId secondMineId = chronological[2];
  gameplay::GameplaySimulation simulation(definition, {.judge = compiledJudge});
  simulation.pressLane(1, {.songTimeMicros = 0});
  simulation.pressLane(2, {.songTimeMicros = 0});

  const auto advanced = simulation.advanceTo(sharedDeadline, 10'000'000);
  require(advanced.transactions.size() == 3 &&
              advanced.transactions[0].noteId == firstMineId &&
              advanced.transactions[0].replayEvent.action ==
                  gameplay::GameplayReplayAction::Mine &&
              advanced.transactions[1].noteId == secondMineId &&
              advanced.transactions[1].replayEvent.action ==
                  gameplay::GameplayReplayAction::Mine &&
              advanced.transactions[2].noteId == normalId &&
              advanced.transactions[2].replayEvent.action ==
                  gameplay::GameplayReplayAction::Miss,
          "equal deadlines use phase order and stable NoteId order");
  require(simulation.replayEvents().size() == 5 &&
              simulation.replayEvents()[2].action ==
                  gameplay::GameplayReplayAction::Mine &&
              simulation.replayEvents()[3].action ==
                  gameplay::GameplayReplayAction::Mine &&
              simulation.replayEvents()[4].action ==
                  gameplay::GameplayReplayAction::Miss,
          "stable phase order is preserved in replay storage");
}

void testAutoplayNormalEmitsOnePressReplayAndVisualRelease() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  auto *measure = new bms_parser::Measure();
  constexpr std::int64_t timingMicros = 1'000'000;
  auto *timeline = addTimeline(*measure, timingMicros);
  timeline->SetNote(1, new bms_parser::Note(17));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const gameplay::NoteId noteId = definition.chronologicalNotes().front();
  gameplay::GameplaySimulation simulation(
      definition, {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
                   .attempt = {.autoPlay = true}});
  const float gaugeBefore = simulation.snapshot().gauge;

  const auto advanced = simulation.advanceTo(timingMicros, 11'000'000);
  require(advanced.transactions.size() == 2 &&
              simulation.automaticResults().size() == 2,
          "autoplay normal emits a press and a release visual transaction");
  const auto &press = advanced.transactions[0];
  const auto &release = advanced.transactions[1];
  require(press.noteId == noteId && press.soundNoteId == noteId &&
              press.hasJudge && press.judge.judgement == PGreat &&
              press.judge.Diff == 0 && press.hasReplayEvent &&
              press.replayEvent.action ==
                  gameplay::GameplayReplayAction::Press &&
              press.hasLaneVisual &&
              press.laneVisual.action == gameplay::LaneVisualAction::Press &&
              press.laneVisual.visualTimeMicros == 11'000'000,
          "autoplay press carries note, sound, PGreat, replay, and visual");
  require(release.noteId == gameplay::kInvalidNoteId &&
              release.soundNoteId == gameplay::kInvalidNoteId &&
              !release.hasJudge && !release.hasReplayEvent &&
              release.hasLaneVisual &&
              release.laneVisual.action ==
                  gameplay::LaneVisualAction::Release &&
              release.laneVisual.lane == 1 &&
              release.laneVisual.visualTimeMicros == 11'000'000,
          "autoplay release is lane-only with no second authority event");
  require(simulation.noteState(noteId).played &&
              simulation.noteState(noteId).playedTimeMicros == timingMicros &&
              !simulation.lanePressed(1) &&
              simulation.snapshot().judgeCounts[PGreat] == 1 &&
              simulation.snapshot().combo == 1 &&
              simulation.snapshot().score == 2 &&
              simulation.snapshot().gauge > gaugeBefore &&
              simulation.replayEvents().size() == 1 &&
              simulation.replayEvents().front() == press.replayEvent,
          "autoplay commits score, gauge, lane state, and one replay once");

  const auto repeated = simulation.advanceTo(timingMicros, 11'000'001);
  require(repeated.transactions.empty() &&
              simulation.snapshot().judgeCounts[PGreat] == 1 &&
              simulation.replayEvents().size() == 1,
          "repeated autoplay advance cannot replay the note");
}

void testAutomaticResultCapacityLatchesWithoutGrowth() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(1, new bms_parser::Note(1));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  gameplay::GameplaySimulation simulation(
      definition, {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
                   .attempt = {.autoPlay = true,
                               .replayCapacity = 2,
                               .automaticResultCapacity = 1}});
  const auto *const storage = simulation.automaticResults().data();
  const auto *const replayStorage = simulation.replayEvents().data();
  const auto *const gaugeHistoryStorage =
      simulation.scoreState().gaugeHistory.data();

  const auto advanced = simulation.advanceTo(1'000'000, 12'000'000);
  require(advanced.transactions.size() == 1 &&
              advanced.transactions.data() == storage &&
              simulation.automaticResults().data() == storage &&
              simulation.automaticResultOverflowed(),
          "automatic result capacity latches without reallocating storage");
  require(
      simulation.noteState(definition.chronologicalNotes().front()).played &&
          simulation.snapshot().judgeCounts[PGreat] == 1 &&
          simulation.replayEvents().size() == 1 &&
          simulation.replayEvents().data() == replayStorage &&
          simulation.scoreState().gaugeHistory.data() == gaugeHistoryStorage,
      "overflow does not roll back the committed autoplay transaction");
}

void testGlobalAutomaticCursorsDoNotReexamineCompletedIdentities() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1'000;
  auto *measure = new bms_parser::Measure();
  for (int index = 0; index < 1'000; ++index) {
    auto *timeline = addTimeline(*measure, 1'000'000 + index * 1'000LL);
    timeline->SetNote(2, new bms_parser::Note(index + 1));
  }
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto compiledJudge = gameplay::CompiledGameplayJudge::from(Judge(1));
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = compiledJudge,
       .attempt = {.replayCapacity = 1'000, .automaticResultCapacity = 1'000}});
  const auto *const storage = simulation.automaticResults().data();
  const std::int64_t target = definition.metadata().finalNoteTimeMicros +
                              compiledJudge.latePoorTimingMicros() + 1;

  const auto advanced = simulation.advanceTo(target, 13'000'000);
  require(advanced.transactions.size() == 1'000 &&
              advanced.transactions.data() == storage &&
              simulation.lastAdvanceStats().notesExamined == 2'000,
          "large advance consumes each global identity once per phase");
  const auto repeated = simulation.advanceTo(target, 13'000'001);
  require(repeated.transactions.empty() &&
              simulation.lastAdvanceStats().notesExamined == 0 &&
              simulation.snapshot().judgeCounts[Poor] == 1'000,
          "completed global cursors examine zero identities on repeat");
}

void testBackwardAdvanceIsIgnoredWithoutRollingBackTime() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 2'000'000);
  timeline->SetNote(1, new bms_parser::Note(1));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto compiledJudge = gameplay::CompiledGameplayJudge::from(Judge(1));
  gameplay::GameplaySimulation simulation(definition, {.judge = compiledJudge});

  const auto forward = simulation.advanceTo(1'500'000, 14'000'000);
  const auto backward = simulation.advanceTo(1'000'000, 14'000'001);
  require(forward.advancedToMicros == 1'500'000 &&
              backward.advancedToMicros == 1'500'000 &&
              backward.transactions.empty() &&
              simulation.lastAdvanceStats().notesExamined == 0,
          "backward advance is a deterministic no-op at the current time");
  const auto backwardPress = simulation.applyPressAt(
      1, 1, {.songTimeMicros = 1'000'000, .laneBeamTimeMicros = 14'000'001});
  require(backwardPress.noteId == gameplay::kInvalidNoteId &&
              !backwardPress.hasReplayEvent && !simulation.lanePressed(1) &&
              simulation.replayEvents().empty(),
          "backward serialized input is rejected without lane mutation");

  const std::int64_t deadline =
      2'000'000 + compiledJudge.latePoorTimingMicros() + 1;
  const auto completed = simulation.advanceTo(deadline, 14'000'002);
  require(completed.transactions.size() == 1 &&
              simulation.snapshot().judgeCounts[Poor] == 1,
          "forward progress remains valid after a rejected backward call");
}
} // namespace

int main() {
  testDefinitionCompilesAutomaticMetadata();
  testDefinitionUsesDefaultGaugeTotalWhenChartOmitsTotal();
  testDefinitionCompilesChronologicalHellChargeHeads();
  testAttemptInitializesConfiguredAndCarriedState();
  testReplayAndGaugeHistoryCapacityLatchWithoutGrowth();
  testNormalLatePoorUsesStrictDeadlineAndIsIdempotent();
  testLandmineDetonatesOrExpiresFromPriorLaneState();
  testSameTimeAutomaticWorkPrecedesPressAndRelease();
  testEqualDeadlineUsesAtTimingPhaseBeforeLatePoor();
  testAutoplayNormalEmitsOnePressReplayAndVisualRelease();
  testAutomaticResultCapacityLatchesWithoutGrowth();
  testGlobalAutomaticCursorsDoNotReexamineCompletedIdentities();
  testBackwardAdvanceIsIgnoredWithoutRollingBackTime();
  return 0;
}
