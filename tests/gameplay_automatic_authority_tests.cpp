#include "scene/play/GameplayDefinition.h"
#include "scene/play/GameplayScoreState.h"
#include "scene/play/GameplaySimulation.h"
#include "scene/play/Judge.h"

#include "bms_parser.hpp"

#include <algorithm>
#include <array>
#include <bit>
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

struct LongPairIds {
  gameplay::NoteId head = gameplay::kInvalidNoteId;
  gameplay::NoteId tail = gameplay::kInvalidNoteId;
};

LongPairIds longPairIds(const gameplay::GameplayDefinition &definition,
                        int lane) {
  LongPairIds result;
  for (const gameplay::NoteId id : definition.laneNotes(lane)) {
    const auto &note = definition.note(id);
    if (note.kind == gameplay::NoteKind::LongHead) {
      result.head = id;
    } else if (note.kind == gameplay::NoteKind::LongTail) {
      result.tail = id;
    }
  }
  require(result.head != gameplay::kInvalidNoteId &&
              result.tail != gameplay::kInvalidNoteId,
          "long-note pair identities are present");
  return result;
}

gameplay::NoteId findNoteId(const gameplay::GameplayDefinition &definition,
                            int lane, std::int64_t timingMicros,
                            gameplay::NoteKind kind) {
  for (const gameplay::NoteId id : definition.chronologicalNotes()) {
    const auto &note = definition.note(id);
    if (note.lane == lane && note.timingMicros == timingMicros &&
        note.kind == kind) {
      return id;
    }
  }
  require(false, "requested stable note identity is present");
  return gameplay::kInvalidNoteId;
}

std::size_t replayActionCount(
    std::span<const gameplay::GameplayReplayEvent> events,
    gameplay::GameplayReplayAction action) {
  return static_cast<std::size_t>(
      std::ranges::count(events, action,
                         &gameplay::GameplayReplayEvent::action));
}

bool sameAttemptSnapshot(const gameplay::GameplayAttemptSnapshot &left,
                         const gameplay::GameplayAttemptSnapshot &right) {
  return left.judgeCounts == right.judgeCounts &&
         left.combo == right.combo && left.maxCombo == right.maxCombo &&
         left.comboBreak == right.comboBreak && left.score == right.score &&
         left.gaugeType == right.gaugeType &&
         left.clearTypeRank == right.clearTypeRank &&
         std::bit_cast<std::uint32_t>(left.gauge) ==
             std::bit_cast<std::uint32_t>(right.gauge);
}

bool sameFinalSummary(const gameplay::GameplayFinalSummary &left,
                      const gameplay::GameplayFinalSummary &right) {
  return left.score == right.score && left.maxCombo == right.maxCombo &&
         left.comboBreak == right.comboBreak &&
         left.totalNotes == right.totalNotes &&
         left.clearTypeRank == right.clearTypeRank &&
         left.fullComboAchieved == right.fullComboAchieved &&
         std::bit_cast<std::uint32_t>(left.finalGauge) ==
             std::bit_cast<std::uint32_t>(right.finalGauge);
}

void requireSameCompleteOutcome(
    const gameplay::GameplayDefinition &definition,
    const gameplay::GameplaySimulation &left,
    const gameplay::GameplaySimulation &right) {
  for (const gameplay::NoteId id : definition.chronologicalNotes()) {
    const auto &leftState = left.noteState(id);
    const auto &rightState = right.noteState(id);
    require(leftState.played == rightState.played &&
                leftState.dead == rightState.dead &&
                leftState.holding == rightState.holding &&
                leftState.playedTimeMicros == rightState.playedTimeMicros &&
                leftState.releaseTimeMicros == rightState.releaseTimeMicros,
            "one-shot and chunked note states are identical");
  }
  for (const auto &lane : definition.lanes()) {
    require(left.lanePressed(lane.lane) == right.lanePressed(lane.lane),
            "one-shot and chunked lane states are identical");
  }
  require(sameAttemptSnapshot(left.snapshot(), right.snapshot()) &&
              left.scoreState().gaugeValues ==
                  right.scoreState().gaugeValues &&
              left.scoreState().gaugeSurvivalFailed ==
                  right.scoreState().gaugeSurvivalFailed &&
              left.scoreState().gaugeHistory ==
                  right.scoreState().gaugeHistory,
          "one-shot and chunked score and gauge state are identical");
  require(std::ranges::equal(left.replayEvents(), right.replayEvents()) &&
              std::ranges::equal(left.hellChargeBalances(),
                                 right.hellChargeBalances()),
          "one-shot and chunked replay and HCN state are identical");
  require(left.terminalReason() == right.terminalReason() &&
              sameAttemptSnapshot(left.terminalSnapshot(),
                                  right.terminalSnapshot()) &&
              sameFinalSummary(left.finalSummary(), right.finalSummary()),
          "one-shot and chunked terminal state is identical");
}

void requireSameHellChargeOutcome(
    const gameplay::GameplaySimulation &left,
    const gameplay::GameplaySimulation &right) {
  const auto leftSnapshot = left.snapshot();
  const auto rightSnapshot = right.snapshot();
  require(leftSnapshot.judgeCounts == rightSnapshot.judgeCounts &&
              leftSnapshot.combo == rightSnapshot.combo &&
              leftSnapshot.maxCombo == rightSnapshot.maxCombo &&
              leftSnapshot.comboBreak == rightSnapshot.comboBreak &&
              leftSnapshot.score == rightSnapshot.score &&
              leftSnapshot.gaugeType == rightSnapshot.gaugeType &&
              leftSnapshot.clearTypeRank == rightSnapshot.clearTypeRank &&
              std::bit_cast<std::uint32_t>(leftSnapshot.gauge) ==
                  std::bit_cast<std::uint32_t>(rightSnapshot.gauge),
          "Hell Charge snapshots are bit-identical");
  require(left.scoreState().gaugeValues == right.scoreState().gaugeValues &&
              left.scoreState().gaugeSurvivalFailed ==
                  right.scoreState().gaugeSurvivalFailed &&
              left.scoreState().gaugeHistory ==
                  right.scoreState().gaugeHistory,
          "Hell Charge gauge state and history are bit-identical");
  require(std::ranges::equal(left.hellChargeBalances(),
                             right.hellChargeBalances()),
          "Hell Charge fixed balances are identical");
  require(std::ranges::equal(left.replayEvents(), right.replayEvents()),
          "Hell Charge replay events are identical");
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

void testCapacityFaultsLatchIndependentlyWithoutGrowth() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 2;
  chart.Meta.KeyMode = 5;
  chart.Meta.HasTotal = true;
  chart.Meta.Total = 260.0;
  auto *measure = new bms_parser::Measure();
  auto *timeline = addTimeline(*measure, 1'000'000);
  timeline->SetNote(1, new bms_parser::Note(1));
  auto *secondTimeline = addTimeline(*measure, 2'000'000);
  secondTimeline->SetNote(2, new bms_parser::Note(2));
  chart.Measures.push_back(measure);
  const auto definition = gameplay::buildGameplayDefinition(chart, 0);

  gameplay::GameplaySimulation replayFault(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .attempt = {.replayCapacity = 0,
                   .automaticResultCapacity = 4,
                   .gaugeHistoryCapacity = 4}});
  const auto *const replayStorage = replayFault.replayEvents().data();
  const auto replayPress =
      replayFault.pressLane(1, {.songTimeMicros = 1'000'000});
  require(replayPress.hasJudge && replayPress.hasReplayEvent &&
              replayPress.replayEvent.score == 2 &&
              replayPress.replayEvent.combo == 1 &&
              replayFault.noteState(
                  definition.chronologicalNotes().front()).played &&
              replayFault.snapshot().judgeCounts[PGreat] == 1,
          "replay overflow finishes the note and score transaction");
  require(replayFault.terminalReason() ==
                  gameplay::GameplayTerminalReason::ReplayCapacityExceeded &&
              replayFault.replayOverflowed() &&
              replayFault.replayEvents().empty() &&
              replayFault.replayEvents().data() == replayStorage &&
              !replayFault.automaticResultOverflowed() &&
              !replayFault.scoreState().gaugeHistoryOverflowed(),
          "replay capacity fault is isolated and never grows storage");

  gameplay::GameplaySimulation gaugeHistoryFault(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .attempt = {.replayCapacity = 4,
                   .automaticResultCapacity = 4,
                   .gaugeHistoryCapacity = 1}});
  const auto firstGaugePress =
      gaugeHistoryFault.pressLane(1, {.songTimeMicros = 1'000'000});
  require(firstGaugePress.hasJudge &&
              gaugeHistoryFault.scoreState().gaugeHistory.size() == 1 &&
              !gaugeHistoryFault.scoreState().gaugeHistoryOverflowed() &&
              !gaugeHistoryFault.terminal(),
          "nonzero gauge-history limit accepts exactly one transaction");
  const auto *const gaugeStorage =
      gaugeHistoryFault.scoreState().gaugeHistory.data();
  const auto gaugeCapacity =
      gaugeHistoryFault.scoreState().gaugeHistory.capacity();
  const auto gaugePress =
      gaugeHistoryFault.pressLane(2, {.songTimeMicros = 2'000'000});
  require(gaugePress.hasJudge && gaugePress.hasReplayEvent &&
              gaugeHistoryFault.replayEvents().size() == 2 &&
              gaugeHistoryFault.snapshot().judgeCounts[PGreat] == 2,
          "gauge-history overflow finishes replay and score mutation");
  require(
      gaugeHistoryFault.terminalReason() ==
              gameplay::GameplayTerminalReason::GaugeHistoryCapacityExceeded &&
          gaugeHistoryFault.scoreState().gaugeHistoryOverflowed() &&
          gaugeHistoryFault.scoreState().gaugeHistory.size() == 1 &&
          gaugeHistoryFault.scoreState().gaugeHistory.capacity() ==
              gaugeCapacity &&
          gaugeHistoryFault.scoreState().gaugeHistory.data() == gaugeStorage &&
          !gaugeHistoryFault.replayOverflowed() &&
          !gaugeHistoryFault.automaticResultOverflowed(),
      "gauge-history capacity fault is isolated and never grows storage");
}

void testSimultaneousFaultPrecedenceAndFirstReasonImmutability() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  auto *measure = new bms_parser::Measure();
  constexpr std::int64_t noteMicros = 1'000'000;
  auto *timeline = addTimeline(*measure, noteMicros);
  timeline->SetNote(1, new bms_parser::Note(1));
  addTimeline(*measure, 4'000'000);
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto compiledJudge = gameplay::CompiledGameplayJudge::from(Judge(1));
  const std::int64_t missMicros =
      noteMicros + compiledJudge.latePoorTimingMicros() + 1;
  const auto zeroStorageAttempt = gameplay::GameplayAttemptOptions{
      .replayCapacity = 0,
      .automaticResultCapacity = 0,
      .gaugeHistoryCapacity = 0,
  };

  auto survivalAttempt = zeroStorageAttempt;
  survivalAttempt.initialGaugeType = GaugeType::Hard;
  survivalAttempt.startingGaugePercent = 1;
  gameplay::GameplaySimulation survival(
      definition, {.judge = compiledJudge, .attempt = survivalAttempt});
  const auto survivalResult =
      survival.advanceTo(missMicros, 12'500'000);
  const auto survivalTerminal = survival.terminalSnapshot();
  require(
      survivalResult.transactions.empty() &&
          survival.terminalReason() ==
              gameplay::GameplayTerminalReason::SurvivalGaugeFailed &&
          survival.noteState(definition.chronologicalNotes().front()).played &&
          survival.noteState(definition.chronologicalNotes().front()).dead &&
          survivalTerminal.judgeCounts[Poor] == 1 &&
          survivalTerminal.comboBreak == 1 &&
          survivalTerminal.gauge == 0.0F &&
          survival.scoreState().gaugeHistoryOverflowed() &&
          survival.replayOverflowed() &&
          survival.automaticResultOverflowed() &&
          survival.scoreState().gaugeHistory.empty() &&
          survival.replayEvents().empty() &&
          survival.automaticResults().empty(),
      "survival outranks simultaneous gauge, replay, and result faults after "
      "the complete miss transaction");
  const auto survivalRepeated =
      survival.advanceTo(missMicros + 1, 12'500'001);
  require(survivalRepeated.transactions.empty() &&
              survival.terminalReason() ==
                  gameplay::GameplayTerminalReason::SurvivalGaugeFailed &&
              sameAttemptSnapshot(survival.terminalSnapshot(),
                                  survivalTerminal),
          "first survival reason remains immutable after simultaneous faults");

  gameplay::GameplaySimulation integrity(
      definition,
      {.judge = compiledJudge, .attempt = zeroStorageAttempt});
  const auto integrityResult =
      integrity.advanceTo(missMicros, 12'600'000);
  const auto integrityTerminal = integrity.terminalSnapshot();
  require(
      integrityResult.transactions.empty() &&
          integrity.terminalReason() ==
              gameplay::GameplayTerminalReason::
                  GaugeHistoryCapacityExceeded &&
          integrity.noteState(definition.chronologicalNotes().front()).played &&
          integrity.noteState(definition.chronologicalNotes().front()).dead &&
          integrityTerminal.judgeCounts[Poor] == 1 &&
          integrityTerminal.comboBreak == 1 && integrityTerminal.gauge > 0.0F &&
          integrity.scoreState().gaugeHistoryOverflowed() &&
          integrity.replayOverflowed() &&
          integrity.automaticResultOverflowed() &&
          integrity.scoreState().gaugeHistory.empty() &&
          integrity.replayEvents().empty() &&
          integrity.automaticResults().empty(),
      "gauge-history fault outranks simultaneous replay and result faults "
      "after the complete miss transaction");
  const auto integrityRepeated =
      integrity.finalizePracticeRange(missMicros + 1, 12'600'001);
  require(integrityRepeated.transactions.empty() &&
              integrity.terminalReason() ==
                  gameplay::GameplayTerminalReason::
                      GaugeHistoryCapacityExceeded &&
              sameAttemptSnapshot(integrity.terminalSnapshot(),
                                  integrityTerminal),
          "first integrity reason remains immutable after later finalization");

  const auto replayResultAttempt = gameplay::GameplayAttemptOptions{
      .replayCapacity = 0,
      .automaticResultCapacity = 0,
      .gaugeHistoryCapacity = 2,
  };
  gameplay::GameplaySimulation replayResult(
      definition,
      {.judge = compiledJudge, .attempt = replayResultAttempt});
  const auto replayResultAdvance =
      replayResult.advanceTo(missMicros, 12'700'000);
  const auto replayResultTerminal = replayResult.terminalSnapshot();
  require(
      replayResultAdvance.transactions.empty() &&
          replayResult.terminalReason() ==
              gameplay::GameplayTerminalReason::ReplayCapacityExceeded &&
          replayResult.noteState(definition.chronologicalNotes().front())
              .played &&
          replayResult.noteState(definition.chronologicalNotes().front()).dead &&
          replayResultTerminal.judgeCounts[Poor] == 1 &&
          replayResultTerminal.comboBreak == 1 &&
          replayResultTerminal.gauge > 0.0F &&
          replayResult.scoreState().gaugeHistory.size() == 1 &&
          !replayResult.scoreState().gaugeHistoryOverflowed() &&
          replayResult.replayOverflowed() &&
          replayResult.automaticResultOverflowed() &&
          replayResult.replayEvents().empty() &&
          replayResult.automaticResults().empty(),
      "replay fault outranks simultaneous result overflow after committing "
      "the note, score, gauge, and bounded payload attempts");
  const auto replayResultRepeated =
      replayResult.advanceTo(missMicros + 1, 12'700'001);
  require(replayResultRepeated.transactions.empty() &&
              replayResult.terminalReason() ==
                  gameplay::GameplayTerminalReason::ReplayCapacityExceeded &&
              sameAttemptSnapshot(replayResult.terminalSnapshot(),
                                  replayResultTerminal) &&
              replayResult.scoreState().gaugeHistory.size() == 1 &&
              replayResult.replayEvents().empty() &&
              replayResult.automaticResults().empty(),
          "first replay reason remains immutable after result overflow");
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
                               .automaticResultCapacity = 0,
                               .gaugeHistoryCapacity = 2}});
  const auto *const storage = simulation.automaticResults().data();
  const auto initialResultSize = simulation.automaticResults().size();
  const auto *const replayStorage = simulation.replayEvents().data();
  const auto *const gaugeHistoryStorage =
      simulation.scoreState().gaugeHistory.data();

  const auto advanced = simulation.advanceTo(1'000'000, 12'000'000);
  require(advanced.transactions.empty() &&
              simulation.automaticResults().data() == storage &&
              simulation.automaticResults().size() == initialResultSize &&
              simulation.automaticResultOverflowed() &&
              simulation.terminalReason() ==
                  gameplay::GameplayTerminalReason::
                      AutomaticResultCapacityExceeded,
          "automatic-result capacity latches without reallocating storage");
  require(
      simulation.noteState(definition.chronologicalNotes().front()).played &&
          simulation.snapshot().judgeCounts[PGreat] == 1 &&
          simulation.replayEvents().size() == 1 &&
          simulation.replayEvents().data() == replayStorage &&
          simulation.scoreState().gaugeHistory.data() == gaugeHistoryStorage &&
          !simulation.replayOverflowed() &&
          !simulation.scoreState().gaugeHistoryOverflowed(),
      "automatic-result overflow finishes the autoplay transaction and "
      "isolates its fault");
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
              simulation.lastAdvanceStats().notesExamined == 2'000 &&
              simulation.snapshot().judgeCounts[Poor] == 1'000 &&
              simulation.terminalReason() ==
                  gameplay::GameplayTerminalReason::ChartComplete,
          "completed global cursors and stats freeze at chart terminal");
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

void testUnpressedLongNoteDeadlineIdentityMatrix() {
  constexpr std::int64_t headMicros = 1'000'000;
  constexpr std::int64_t tailMicros = 3'000'000;
  const auto compiledJudge = gameplay::CompiledGameplayJudge::from(Judge(1));
  const std::int64_t headDeadline =
      headMicros + compiledJudge.latePoorTimingMicros() + 1;
  require(headDeadline < tailMicros,
          "long-note miss matrix exercises an early future tail");

  {
    bms_parser::Chart chart;
    chart.Meta.TotalNotes = 1;
    auto *measure = new bms_parser::Measure();
    addLongNote(*measure, headMicros, tailMicros, 1,
                bms_parser::LongNoteType::LongNote);
    chart.Measures.push_back(measure);
    const auto definition = gameplay::buildGameplayDefinition(chart, 0);
    const auto ids = longPairIds(definition, 1);
    gameplay::GameplaySimulation simulation(
        definition,
        {.judge = compiledJudge,
         .attempt = {.replayCapacity = 8, .automaticResultCapacity = 8}});
    const float gaugeBefore = simulation.snapshot().gauge;

    const auto advanced = simulation.advanceTo(headDeadline, 15'000'000);
    require(advanced.transactions.size() == 1 &&
                advanced.transactions.front().noteId == ids.head &&
                advanced.transactions.front().judge.judgement == Poor,
            "unpressed Classic head emits one head-identity Poor");
    require(simulation.noteState(ids.head).played &&
                simulation.noteState(ids.head).dead &&
                !simulation.noteState(ids.head).holding &&
                simulation.noteState(ids.tail).played &&
                !simulation.noteState(ids.tail).dead &&
                !simulation.noteState(ids.tail).holding &&
                simulation.noteState(ids.tail).playedTimeMicros == headDeadline,
            "Classic miss resolves the pair without a second dead identity");
    const auto snapshot = simulation.snapshot();
    require(snapshot.judgeCounts[Poor] == 1 && snapshot.combo == 0 &&
                snapshot.comboBreak == 1 && snapshot.gauge < gaugeBefore &&
                simulation.replayEvents().size() == 1 &&
                simulation.replayEvents().front().noteId == ids.head &&
                simulation.replayEvents().front().combo == snapshot.combo &&
                simulation.replayEvents().front().gauge == snapshot.gauge,
            "Classic miss commits one score, gauge, and post-state replay");
  }

  for (const auto type : {bms_parser::LongNoteType::ChargeNote,
                          bms_parser::LongNoteType::HellChargeNote}) {
    bms_parser::Chart chart;
    chart.Meta.TotalNotes = 2;
    auto *measure = new bms_parser::Measure();
    addLongNote(*measure, headMicros, tailMicros, 1, type);
    chart.Measures.push_back(measure);
    const auto definition = gameplay::buildGameplayDefinition(chart, 0);
    const auto ids = longPairIds(definition, 1);
    gameplay::GameplaySimulation simulation(
        definition,
        {.judge = compiledJudge,
         .attempt = {.replayCapacity = 8, .automaticResultCapacity = 8}});
    const float gaugeBefore = simulation.snapshot().gauge;

    const auto advanced = simulation.advanceTo(headDeadline, 15'100'000);
    const std::size_t expectedGaugeTransactions =
        type == bms_parser::LongNoteType::HellChargeNote ? 2 : 0;
    require(
        advanced.transactions.size() == expectedGaugeTransactions + 2 &&
            advanced.transactions[expectedGaugeTransactions].noteId ==
                ids.head &&
            advanced.transactions[expectedGaugeTransactions + 1].noteId ==
                ids.tail &&
            advanced.transactions[expectedGaugeTransactions]
                    .judge.judgement == Poor &&
            advanced.transactions[expectedGaugeTransactions + 1]
                    .judge.judgement == Poor &&
            advanced.transactions[expectedGaugeTransactions + 1].judge.Diff ==
                headDeadline - headMicros &&
            advanced.transactions[expectedGaugeTransactions + 1]
                    .replayEvent.noteTimeMicros == tailMicros &&
            advanced.transactions[expectedGaugeTransactions + 1]
                    .replayEvent.judgeTimeMicros == headDeadline,
        "unpressed Charge/HCN pair emits one Poor per stable identity");
    require(simulation.noteState(ids.head).played &&
                simulation.noteState(ids.head).dead &&
                simulation.noteState(ids.tail).played &&
                !simulation.noteState(ids.tail).dead &&
                simulation.noteState(ids.tail).playedTimeMicros ==
                    headDeadline &&
                !simulation.noteState(ids.head).holding &&
                !simulation.noteState(ids.tail).holding,
            "early Charge/HCN tail remains played but not dead");
    const auto snapshot = simulation.snapshot();
    const std::size_t expectedGaugeReplays = expectedGaugeTransactions;
    const auto replays = simulation.replayEvents();
    require(snapshot.judgeCounts[Poor] == 2 && snapshot.combo == 0 &&
                snapshot.comboBreak == 2 && snapshot.gauge < gaugeBefore &&
                replays.size() == expectedGaugeReplays + 2 &&
                replayActionCount(replays,
                                  gameplay::GameplayReplayAction::Gauge) ==
                    expectedGaugeReplays &&
                replays[expectedGaugeReplays].noteId == ids.head &&
                replays[expectedGaugeReplays + 1].noteId == ids.tail &&
                replays[expectedGaugeReplays + 1].combo == snapshot.combo &&
                replays[expectedGaugeReplays + 1].gauge == snapshot.gauge,
            "Charge/HCN misses commit separately ordered post-state replays");

    const auto atFutureTail = simulation.advanceTo(tailMicros, 15'100'001);
    require((type == bms_parser::LongNoteType::HellChargeNote
                 ? std::ranges::all_of(
                       atFutureTail.transactions,
                       [](const gameplay::GameplayInputResult &result) {
                         return !result.hasJudge && result.hasReplayEvent &&
                                result.replayEvent.action ==
                                    gameplay::GameplayReplayAction::Gauge;
                       }) &&
                       !atFutureTail.transactions.empty()
                 : atFutureTail.transactions.empty()) &&
                simulation.noteState(ids.tail).played &&
                !simulation.noteState(ids.tail).dead &&
                simulation.snapshot().judgeCounts[Poor] == 2,
            "early-resolved Charge/HCN tail stays live through its timing");
  }
}

void testHeldLongNoteAutomaticReleaseMatrix() {
  constexpr std::int64_t headMicros = 1'000'000;
  constexpr std::int64_t tailMicros = 2'000'000;
  const auto compiledJudge = gameplay::CompiledGameplayJudge::from(Judge(1));

  {
    bms_parser::Chart chart;
    chart.Meta.TotalNotes = 1;
    auto *measure = new bms_parser::Measure();
    addLongNote(*measure, headMicros, tailMicros, 1,
                bms_parser::LongNoteType::LongNote);
    chart.Measures.push_back(measure);
    const auto definition = gameplay::buildGameplayDefinition(chart, 0);
    const auto ids = longPairIds(definition, 1);
    gameplay::GameplaySimulation simulation(definition,
                                            {.judge = compiledJudge});
    const auto press = simulation.pressLane(1, {.songTimeMicros = headMicros});
    require(press.noteId == ids.head && !press.hasJudge &&
                simulation.noteState(ids.head).holding &&
                simulation.noteState(ids.tail).holding,
            "Classic head starts one deferred held judgement");

    const auto released = simulation.advanceTo(tailMicros, 16'000'000);
    require(released.transactions.size() == 1 &&
                released.transactions.front().noteId == ids.tail &&
                released.transactions.front().hasJudge &&
                released.transactions.front().judge.judgement == PGreat &&
                released.transactions.front().replayEvent.action ==
                    gameplay::GameplayReplayAction::Release &&
                !released.transactions.front().hasLaneVisual,
            "held Classic tail auto-releases with the combined judgement");
    require(simulation.noteState(ids.head).played &&
                !simulation.noteState(ids.head).dead &&
                !simulation.noteState(ids.head).holding &&
                simulation.noteState(ids.tail).played &&
                !simulation.noteState(ids.tail).dead &&
                !simulation.noteState(ids.tail).holding &&
                simulation.noteState(ids.tail).releaseTimeMicros ==
                    tailMicros &&
                simulation.lanePressed(1) &&
                simulation.snapshot().judgeCounts[PGreat] == 1 &&
                simulation.replayEvents().size() == 2,
            "Classic automatic release resolves pair state once without faking "
            "input");
  }

  for (const auto type : {bms_parser::LongNoteType::ChargeNote,
                          bms_parser::LongNoteType::HellChargeNote}) {
    bms_parser::Chart chart;
    chart.Meta.TotalNotes = 2;
    auto *measure = new bms_parser::Measure();
    addLongNote(*measure, headMicros, tailMicros, 1, type);
    chart.Measures.push_back(measure);
    const auto definition = gameplay::buildGameplayDefinition(chart, 0);
    const auto ids = longPairIds(definition, 1);
    gameplay::GameplaySimulation simulation(definition,
                                            {.judge = compiledJudge});
    const auto press = simulation.pressLane(1, {.songTimeMicros = headMicros});
    const auto atTail = simulation.advanceTo(tailMicros, 16'100'000);
    const std::size_t expectedGaugeTransactions =
        type == bms_parser::LongNoteType::HellChargeNote ? 4 : 0;
    require(press.hasJudge && press.judge.judgement == PGreat &&
                atTail.transactions.size() == expectedGaugeTransactions &&
                std::ranges::all_of(
                    atTail.transactions,
                    [](const gameplay::GameplayInputResult &result) {
                      return !result.hasJudge && result.hasReplayEvent &&
                             result.replayEvent.action ==
                                 gameplay::GameplayReplayAction::Gauge;
                    }) &&
                simulation.noteState(ids.head).holding &&
                simulation.noteState(ids.tail).holding &&
                !simulation.noteState(ids.tail).played &&
                simulation.snapshot().judgeCounts[PGreat] == 1,
            "manual Charge/HCN remains holding at tail timing");

    const auto release =
        simulation.releaseLane(1, {.songTimeMicros = tailMicros});
    require(release.noteId == ids.tail && release.hasJudge &&
                release.judge.judgement == PGreat &&
                !simulation.noteState(ids.head).holding &&
                !simulation.noteState(ids.tail).holding &&
                simulation.noteState(ids.tail).played &&
                simulation.snapshot().judgeCounts[PGreat] == 2,
            "manual Charge/HCN resolves only on physical release");

    gameplay::GameplaySimulation lateSimulation(definition,
                                                {.judge = compiledJudge});
    lateSimulation.pressLane(1, {.songTimeMicros = headMicros});
    const std::int64_t tailDeadline =
        tailMicros + compiledJudge.latePoorTimingMicros() + 1;
    const auto late = lateSimulation.advanceTo(tailDeadline, 16'100'001);
    require(late.transactions.size() == expectedGaugeTransactions + 1 &&
                late.transactions.back().noteId == ids.tail &&
                late.transactions.back().judge.judgement == Poor &&
                lateSimulation.noteState(ids.tail).played &&
                lateSimulation.noteState(ids.tail).dead &&
                !lateSimulation.noteState(ids.head).holding &&
                !lateSimulation.noteState(ids.tail).holding &&
                lateSimulation.snapshot().judgeCounts[PGreat] == 1 &&
                lateSimulation.snapshot().judgeCounts[Poor] == 1,
            "held Charge/HCN tail eventually resolves as its own late Poor");
  }
}

void testAutoplayChargeAndHellChargeReleaseAtTiming() {
  constexpr std::int64_t headMicros = 1'000'000;
  constexpr std::int64_t tailMicros = 2'000'000;
  for (const auto type : {bms_parser::LongNoteType::ChargeNote,
                          bms_parser::LongNoteType::HellChargeNote}) {
    bms_parser::Chart chart;
    chart.Meta.TotalNotes = 2;
    auto *measure = new bms_parser::Measure();
    addLongNote(*measure, headMicros, tailMicros, 1, type);
    chart.Measures.push_back(measure);
    const auto definition = gameplay::buildGameplayDefinition(chart, 0);
    const auto ids = longPairIds(definition, 1);
    gameplay::GameplaySimulation simulation(
        definition, {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
                     .attempt = {.autoPlay = true}});

    const auto advanced = simulation.advanceTo(tailMicros, 17'000'000);
    const std::size_t expectedGaugeTransactions =
        type == bms_parser::LongNoteType::HellChargeNote ? 4 : 0;
    const std::size_t tailIndex = expectedGaugeTransactions + 1;
    require(
        advanced.transactions.size() == expectedGaugeTransactions + 2 &&
            advanced.transactions[0].noteId == ids.head &&
            advanced.transactions[0].soundNoteId == ids.head &&
            advanced.transactions[0].judge.judgement == PGreat &&
            advanced.transactions[0].laneVisual.action ==
                gameplay::LaneVisualAction::Press &&
            advanced.transactions[tailIndex].noteId == ids.tail &&
            advanced.transactions[tailIndex].soundNoteId ==
                gameplay::kInvalidNoteId &&
            advanced.transactions[tailIndex].judge.judgement == PGreat &&
            advanced.transactions[tailIndex].laneVisual.action ==
                gameplay::LaneVisualAction::Release,
        "autoplay Charge/HCN presses the head and releases the tail at timing");
    require(simulation.noteState(ids.head).played &&
                simulation.noteState(ids.tail).played &&
                !simulation.noteState(ids.head).holding &&
                !simulation.noteState(ids.tail).holding &&
                !simulation.lanePressed(1) &&
                simulation.snapshot().judgeCounts[PGreat] == 2 &&
                simulation.snapshot().combo == 2 &&
                simulation.snapshot().score == 4 &&
                simulation.replayEvents().size() ==
                    expectedGaugeTransactions + 2 &&
                simulation.replayEvents()[0].noteId == ids.head &&
                simulation.replayEvents()[tailIndex].noteId == ids.tail,
            "autoplay Charge/HCN commits two identity judgements and replays");
  }
}

void testStartInitializationResolvesPreStartWorkWithoutTransactions() {
  constexpr std::int64_t startMicros = 1'000'000;
  constexpr std::int64_t endMicros = 3'000'000;
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 7;
  auto *measure = new bms_parser::Measure();
  auto *normalBefore = addTimeline(*measure, 100'000);
  normalBefore->SetNote(1, new bms_parser::Note(1));
  auto *mineBefore = addTimeline(*measure, 200'000);
  mineBefore->SetLandmineNote(2, new bms_parser::LandmineNote(5.0F));
  addLongNote(*measure, 300'000, startMicros + 10'000, 3,
              bms_parser::LongNoteType::LongNote);
  addLongNote(*measure, 400'000, 500'000, 4,
              bms_parser::LongNoteType::ChargeNote);
  auto *atStart = addTimeline(*measure, startMicros);
  atStart->SetNote(5, new bms_parser::Note(5));
  auto *future = addTimeline(*measure, 2'000'000);
  future->SetNote(6, new bms_parser::Note(6));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto compiledJudge = gameplay::CompiledGameplayJudge::from(Judge(1));
  const auto classic = longPairIds(definition, 3);
  const auto charge = longPairIds(definition, 4);
  const auto normalBeforeId =
      findNoteId(definition, 1, 100'000, gameplay::NoteKind::Normal);
  const auto mineBeforeId =
      findNoteId(definition, 2, 200'000, gameplay::NoteKind::Landmine);
  const auto atStartId =
      findNoteId(definition, 5, startMicros, gameplay::NoteKind::Normal);
  const auto futureId =
      findNoteId(definition, 6, 2'000'000, gameplay::NoteKind::Normal);
  gameplay::GameplaySimulation simulation(
      definition, {.judge = compiledJudge,
                   .allowedNoteRange =
                       gameplay::GameplayTimeRange{.startMicros = startMicros,
                                                   .endMicros = endMicros},
                   .attempt = {.startingGaugePercent = 37,
                               .carriedCombo = 3,
                               .carriedMaxCombo = 4,
                               .replayCapacity = 16,
                               .automaticResultCapacity = 16}});

  const auto initialized = simulation.snapshot();
  require(simulation.noteState(normalBeforeId).played &&
              simulation.noteState(normalBeforeId).dead &&
              simulation.noteState(normalBeforeId).playedTimeMicros ==
                  startMicros &&
              simulation.noteState(mineBeforeId).played &&
              simulation.noteState(mineBeforeId).dead &&
              simulation.noteState(mineBeforeId).playedTimeMicros ==
                  startMicros,
          "start initialization resolves pre-start normals and mines");
  require(simulation.noteState(classic.head).played &&
              simulation.noteState(classic.head).dead &&
              simulation.noteState(classic.tail).played &&
              simulation.noteState(classic.tail).dead &&
              simulation.noteState(classic.tail).playedTimeMicros ==
                  startMicros &&
              simulation.noteState(charge.head).played &&
              simulation.noteState(charge.head).dead &&
              simulation.noteState(charge.tail).played &&
              simulation.noteState(charge.tail).dead,
          "start initialization resolves both identities of crossing and old "
          "pairs");
  require(!simulation.noteState(atStartId).played &&
              !simulation.noteState(futureId).played &&
              initialized.judgeCounts[Poor] == 0 && initialized.combo == 3 &&
              initialized.maxCombo == 4 && initialized.comboBreak == 0 &&
              initialized.gauge == 37.0F &&
              simulation.scoreState().gaugeHistory.empty() &&
              simulation.replayEvents().empty() &&
              simulation.automaticResults().empty(),
          "start initialization has no score, gauge, replay, or boundary side "
          "effects");

  const auto backward = simulation.advanceTo(startMicros - 1, 18'000'000);
  require(backward.transactions.empty() &&
              backward.advancedToMicros == startMicros &&
              simulation.lastAdvanceStats().notesExamined == 0,
          "start initialization establishes the monotonic attempt position");
  const std::int64_t atStartDeadline =
      startMicros + compiledJudge.latePoorTimingMicros() + 1;
  const auto advanced = simulation.advanceTo(atStartDeadline, 18'000'001);
  require(advanced.transactions.size() == 1 &&
              advanced.transactions.front().noteId == atStartId &&
              simulation.snapshot().judgeCounts[Poor] == 1 &&
              simulation.replayEvents().size() == 1 &&
              simulation.lastAdvanceStats().notesExamined == 3 &&
              simulation.noteState(classic.tail).dead &&
              !simulation.noteState(futureId).played,
          "both automatic cursors skip obsolete work without replaying it");
}

void testPracticeFinalizationMatchesHalfOpenIdentityRules() {
  constexpr std::int64_t startMicros = 500'000;
  constexpr std::int64_t endMicros = 1'000'000;
  constexpr std::int64_t finalizationMicros = endMicros - 1;
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 10;
  auto *measure = new bms_parser::Measure();
  auto *beforeStart = addTimeline(*measure, startMicros - 1);
  beforeStart->SetNote(1, new bms_parser::Note(1));
  addLongNote(*measure, endMicros - 60, endMicros + 20, 6,
              bms_parser::LongNoteType::LongNote);
  addLongNote(*measure, endMicros - 50, endMicros - 45, 0,
              bms_parser::LongNoteType::LongNote);
  addLongNote(*measure, endMicros - 40, endMicros + 10, 5,
              bms_parser::LongNoteType::LongNote);
  addLongNote(*measure, endMicros - 30, endMicros - 25, 4,
              bms_parser::LongNoteType::HellChargeNote);
  addLongNote(*measure, endMicros - 20, endMicros - 10, 3,
              bms_parser::LongNoteType::ChargeNote);
  auto *mineTimeline = addTimeline(*measure, endMicros - 2);
  mineTimeline->SetLandmineNote(2, new bms_parser::LandmineNote(5.0F));
  auto *lastValid = addTimeline(*measure, endMicros - 1);
  lastValid->SetNote(1, new bms_parser::Note(2));
  auto *atEnd = addTimeline(*measure, endMicros);
  atEnd->SetNote(7, new bms_parser::Note(3));
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto heldClassic = longPairIds(definition, 6);
  const auto fullClassic = longPairIds(definition, 0);
  const auto crossingClassic = longPairIds(definition, 5);
  const auto hellCharge = longPairIds(definition, 4);
  const auto charge = longPairIds(definition, 3);
  const auto mineId =
      findNoteId(definition, 2, endMicros - 2, gameplay::NoteKind::Landmine);
  const auto lastValidId =
      findNoteId(definition, 1, endMicros - 1, gameplay::NoteKind::Normal);
  const auto atEndId =
      findNoteId(definition, 7, endMicros, gameplay::NoteKind::Normal);
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
       .allowedNoteRange =
           gameplay::GameplayTimeRange{.startMicros = startMicros,
                                       .endMicros = endMicros},
       .attempt = {.replayCapacity = 32, .automaticResultCapacity = 32}});
  const auto heldPress = simulation.pressLane(
      6, {.songTimeMicros = endMicros - 60, .laneBeamTimeMicros = 19'000'000});
  require(heldPress.noteId == heldClassic.head && !heldPress.hasJudge &&
              simulation.noteState(heldClassic.head).holding,
          "practice finalization setup holds a crossing Classic head");
  const auto *const resultStorage = simulation.automaticResults().data();
  const auto *const replayStorage = simulation.replayEvents().data();
  const auto *const gaugeStorage = simulation.scoreState().gaugeHistory.data();

  const auto finalized =
      simulation.finalizePracticeRange(finalizationMicros, 19'000'001);
  const std::array expectedIds{
      fullClassic.head, crossingClassic.head, hellCharge.head, hellCharge.tail,
      charge.head,      charge.tail,          lastValidId};
  require(finalized.transactions.size() == expectedIds.size() &&
              finalized.transactions.data() == resultStorage,
          "practice finalization emits one fixed-storage transaction per "
          "judged identity");
  for (std::size_t index = 0; index < expectedIds.size(); ++index) {
    require(finalized.transactions[index].noteId == expectedIds[index] &&
                finalized.transactions[index].hasJudge &&
                finalized.transactions[index].judge.judgement == Poor &&
                finalized.transactions[index].hasReplayEvent &&
                finalized.transactions[index].replayEvent.action ==
                    gameplay::GameplayReplayAction::Miss,
            "practice misses preserve chronological stable identity order");
  }
  require(simulation.noteState(fullClassic.head).played &&
              simulation.noteState(fullClassic.head).dead &&
              simulation.noteState(fullClassic.tail).played &&
              !simulation.noteState(fullClassic.tail).dead &&
              simulation.noteState(crossingClassic.head).played &&
              simulation.noteState(crossingClassic.head).dead &&
              !simulation.noteState(crossingClassic.tail).played,
          "practice Classic rules collapse an eligible pair and preserve "
          "crossing tail");
  require(simulation.noteState(charge.head).played &&
              simulation.noteState(charge.head).dead &&
              simulation.noteState(charge.tail).played &&
              simulation.noteState(charge.tail).dead &&
              simulation.noteState(hellCharge.head).played &&
              simulation.noteState(hellCharge.head).dead &&
              simulation.noteState(hellCharge.tail).played &&
              simulation.noteState(hellCharge.tail).dead,
          "practice Charge and HCN identities finalize separately");
  require(simulation.noteState(heldClassic.head).played &&
              !simulation.noteState(heldClassic.head).dead &&
              !simulation.noteState(heldClassic.head).holding &&
              !simulation.noteState(heldClassic.tail).played &&
              !simulation.noteState(heldClassic.tail).holding &&
              !simulation.noteState(mineId).played &&
              !simulation.noteState(mineId).dead &&
              !simulation.noteState(atEndId).played &&
              !simulation.noteState(atEndId).dead,
          "practice finalization clears crossing hold and ignores mines and "
          "end boundary");
  const auto snapshot = simulation.snapshot();
  require(
      snapshot.judgeCounts[Poor] == 7 && snapshot.combo == 0 &&
          snapshot.comboBreak == 7 && simulation.replayEvents().size() == 8 &&
          simulation.replayEvents().data() == replayStorage &&
          simulation.scoreState().gaugeHistory.data() == gaugeStorage,
      "practice identity misses commit bounded score, gauge, and replay state");
  require(simulation.terminal() &&
              simulation.terminalReason() ==
                  gameplay::GameplayTerminalReason::PracticeComplete &&
              sameAttemptSnapshot(simulation.terminalSnapshot(), snapshot),
          "practice completion latches only after its final miss transaction");

  const auto duplicate =
      simulation.finalizePracticeRange(finalizationMicros, 19'000'002);
  require(duplicate.transactions.empty() &&
              simulation.automaticResults().data() == resultStorage &&
              simulation.replayEvents().size() == 8 &&
              simulation.snapshot().judgeCounts[Poor] == 7,
          "practice finalization rejects repeated calls without allocation or "
          "mutation");
}

void testHellChargeStrictThresholdsAndSerialInputState() {
  constexpr std::int64_t headMicros = 1'000'000;
  constexpr std::int64_t tailMicros = 2'000'000;
  constexpr std::int64_t tickMicros = 200'000;
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 2;
  auto *measure = new bms_parser::Measure();
  addLongNote(*measure, headMicros, tailMicros, 1,
              bms_parser::LongNoteType::HellChargeNote);
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto ids = longPairIds(definition, 1);
  const auto compiledJudge = gameplay::CompiledGameplayJudge::from(Judge(1));

  gameplay::GameplaySimulation gaining(
      definition,
      {.judge = compiledJudge,
       .attempt = {.replayCapacity = 32, .automaticResultCapacity = 32}});
  const auto headPress = gaining.applyPressAt(
      1, 1, {.songTimeMicros = headMicros, .laneBeamTimeMicros = 20'000'000});
  require(headPress.noteId == ids.head && headPress.hasJudge &&
              headPress.judge.judgement == PGreat,
          "Hell Charge setup presses the head exactly");
  const auto gainReplayCount = gaining.replayEvents().size();
  gaining.advanceTo(headMicros + tickMicros, 20'000'001);
  require(gaining.hellChargeBalances()[ids.head] == tickMicros &&
              gaining.replayEvents().size() == gainReplayCount,
          "exactly positive 200000us stores balance without a tick");
  const auto gainBefore = gaining.snapshot();
  const auto gainHistoryCount = gaining.scoreState().gaugeHistory.size();
  auto expectedGainGauge = gaining.scoreState();
  expectedGainGauge.applyGaugeJudgementRate(Great, 0.5F);
  const auto gainTickAdvance =
      gaining.advanceTo(headMicros + tickMicros + 1, 20'000'002);
  const auto gainAfter = gaining.snapshot();
  const auto &gainTick = gaining.replayEvents().back();
  require(gainTickAdvance.transactions.size() == 1 &&
              !gainTickAdvance.transactions.front().hasJudge &&
              gainTickAdvance.transactions.front().judge.judgement == Great &&
              gainTickAdvance.transactions.front().hasReplayEvent &&
              gainTickAdvance.transactions.front().replayEvent == gainTick &&
              gaining.hellChargeBalances()[ids.head] == 1 &&
              gaining.replayEvents().size() == gainReplayCount + 1 &&
              gainTick.action == gameplay::GameplayReplayAction::Gauge &&
              gainTick.noteId == gameplay::kInvalidNoteId &&
              gainTick.lane == -1 && gainTick.noteTimeMicros == -1 &&
              gainTick.songTimeMicros == headMicros + tickMicros + 1 &&
              gainTick.judgeTimeMicros == headMicros + tickMicros + 1 &&
              gainTick.judgement == Great && gainTick.diffMicros == 0,
          "positive 200001us emits one timestamped half-rate Great tick");
  require(gainAfter.judgeCounts == gainBefore.judgeCounts &&
              gainAfter.combo == gainBefore.combo &&
              gainAfter.maxCombo == gainBefore.maxCombo &&
              gainAfter.comboBreak == gainBefore.comboBreak &&
              gainAfter.score == gainBefore.score &&
              gaining.scoreState().gaugeValues ==
                  expectedGainGauge.gaugeValues &&
              gaining.scoreState().gaugeHistory.size() ==
                  gainHistoryCount + 1 &&
              std::bit_cast<std::uint32_t>(
                  gaining.scoreState().gaugeHistory.back()) ==
                  std::bit_cast<std::uint32_t>(gainTick.gauge) &&
              std::bit_cast<std::uint32_t>(gainTick.gauge) ==
                  std::bit_cast<std::uint32_t>(gainAfter.gauge) &&
              gainTick.gaugeType == gainAfter.gaugeType &&
              gainTick.combo == gainAfter.combo &&
              gainTick.score == gainAfter.score,
          "Great gauge tick has complete post-state without judge/combo edits");

  gameplay::GameplaySimulation releasedAtT(
      definition,
      {.judge = compiledJudge,
       .attempt = {.replayCapacity = 32, .automaticResultCapacity = 32}});
  releasedAtT.applyPressAt(1, 1, {.songTimeMicros = headMicros});
  releasedAtT.applyReleaseAt(
      1, {.songTimeMicros = headMicros + tickMicros});
  require(releasedAtT.hellChargeBalances()[ids.head] == tickMicros &&
              !releasedAtT.lanePressed(1),
          "release at T integrates through T with the prior held state");
  releasedAtT.advanceTo(headMicros + tickMicros + 1, 20'000'003);
  require(releasedAtT.hellChargeBalances()[ids.head] == tickMicros - 1 &&
              replayActionCount(releasedAtT.replayEvents(),
                                gameplay::GameplayReplayAction::Gauge) == 0,
          "release changes Hell Charge direction only after its timestamp");

  gameplay::GameplaySimulation pressedAtT(
      definition,
      {.judge = compiledJudge,
       .attempt = {.replayCapacity = 32, .automaticResultCapacity = 32}});
  pressedAtT.advanceTo(headMicros + 100'000, 20'000'004);
  require(pressedAtT.hellChargeBalances()[ids.head] == -100'000,
          "unheld Hell Charge loses time before a later press");
  pressedAtT.applyPressAt(1, 1, {.songTimeMicros = headMicros + 100'000});
  require(pressedAtT.hellChargeBalances()[ids.head] == -100'000 &&
              pressedAtT.lanePressed(1),
          "press at T integrates through T before changing lane state");
  pressedAtT.advanceTo(headMicros + tickMicros, 20'000'005);
  require(pressedAtT.hellChargeBalances()[ids.head] == 0,
          "press affects only the later Hell Charge interval");

  gameplay::GameplaySimulation damaged(
      definition,
      {.judge = compiledJudge,
       .attempt = {.replayCapacity = 32, .automaticResultCapacity = 32}});
  damaged.applyPressAt(1, 1, {.songTimeMicros = headMicros});
  const auto earlyRelease =
      damaged.applyReleaseAt(1, {.songTimeMicros = headMicros});
  require(earlyRelease.noteId == ids.tail &&
              damaged.noteState(ids.tail).played &&
              !damaged.noteState(ids.tail).dead &&
              !damaged.noteState(ids.head).holding,
          "early Hell Charge release leaves a played live tail");
  const auto damageReplayCount = damaged.replayEvents().size();
  damaged.advanceTo(headMicros + tickMicros, 20'000'006);
  require(damaged.hellChargeBalances()[ids.head] == -tickMicros &&
              damaged.replayEvents().size() == damageReplayCount,
          "exactly negative 200000us stores balance without a tick");
  const auto damageBefore = damaged.snapshot();
  const auto damageHistoryCount = damaged.scoreState().gaugeHistory.size();
  auto expectedDamageGauge = damaged.scoreState();
  expectedDamageGauge.applyGaugeJudgementRate(Bad, 0.5F);
  const auto damageTickAdvance =
      damaged.advanceTo(headMicros + tickMicros + 1, 20'000'007);
  const auto damageAfter = damaged.snapshot();
  const auto &damageTick = damaged.replayEvents().back();
  require(damageTickAdvance.transactions.size() == 1 &&
              !damageTickAdvance.transactions.front().hasJudge &&
              damageTickAdvance.transactions.front().judge.judgement == Bad &&
              damageTickAdvance.transactions.front().hasReplayEvent &&
              damageTickAdvance.transactions.front().replayEvent ==
                  damageTick &&
              damaged.hellChargeBalances()[ids.head] == -1 &&
              damaged.replayEvents().size() == damageReplayCount + 1 &&
              damageTick.action == gameplay::GameplayReplayAction::Gauge &&
              damageTick.noteId == gameplay::kInvalidNoteId &&
              damageTick.lane == -1 && damageTick.noteTimeMicros == -1 &&
              damageTick.songTimeMicros == headMicros + tickMicros + 1 &&
              damageTick.judgeTimeMicros == headMicros + tickMicros + 1 &&
              damageTick.judgement == Bad && damageTick.diffMicros == 0,
          "negative 200001us emits one timestamped half-rate Bad tick");
  require(damageAfter.judgeCounts == damageBefore.judgeCounts &&
              damageAfter.combo == damageBefore.combo &&
              damageAfter.maxCombo == damageBefore.maxCombo &&
              damageAfter.comboBreak == damageBefore.comboBreak &&
              damageAfter.score == damageBefore.score &&
              damaged.scoreState().gaugeValues ==
                  expectedDamageGauge.gaugeValues &&
              damaged.scoreState().gaugeHistory.size() ==
                  damageHistoryCount + 1 &&
              std::bit_cast<std::uint32_t>(
                  damaged.scoreState().gaugeHistory.back()) ==
                  std::bit_cast<std::uint32_t>(damageTick.gauge) &&
              std::bit_cast<std::uint32_t>(damageTick.gauge) ==
                  std::bit_cast<std::uint32_t>(damageAfter.gauge) &&
              damageTick.gaugeType == damageAfter.gaugeType &&
              damageTick.combo == damageAfter.combo &&
              damageTick.score == damageAfter.score,
          "Bad gauge tick has complete post-state without judge/combo edits");

  damaged.applyPressAt(
      1, 1, {.songTimeMicros = headMicros + tickMicros + 1});
  require(damaged.lanePressed(1) &&
              !damaged.noteState(ids.head).holding &&
              damaged.noteState(ids.tail).played &&
              !damaged.noteState(ids.tail).dead,
          "regrab uses lane state without restoring long-note holding");
  const auto regrabGaugeCount = replayActionCount(
      damaged.replayEvents(), gameplay::GameplayReplayAction::Gauge);
  damaged.advanceTo(headMicros + 2 * tickMicros + 2, 20'000'008);
  require(damaged.hellChargeBalances()[ids.head] == tickMicros &&
              replayActionCount(damaged.replayEvents(),
                                gameplay::GameplayReplayAction::Gauge) ==
                  regrabGaugeCount,
          "regrab gain at the exact positive boundary does not tick");
  damaged.advanceTo(headMicros + 2 * tickMicros + 3, 20'000'009);
  require(damaged.hellChargeBalances()[ids.head] == 1 &&
              damaged.replayEvents().back().action ==
                  gameplay::GameplayReplayAction::Gauge &&
              damaged.replayEvents().back().judgement == Great,
          "regrab gains and emits Great while holding remains false");
  damaged.advanceTo(tailMicros, 20'000'010);
  require(damaged.noteState(ids.tail).played &&
              !damaged.noteState(ids.tail).dead &&
              damaged.hellChargeBalances()[ids.head] != 0,
          "early-resolved played live tail integrates through tail timing");
  damaged.advanceTo(tailMicros + 1, 20'000'011);
  require(damaged.hellChargeBalances()[ids.head] == 0,
          "inactive Hell Charge balance resets on the next positive interval");
}

void testMultipleHellChargeCrossingsUseTimeThenNoteIdOrder() {
  constexpr std::int64_t headMicros = 1'000'000;
  constexpr std::int64_t tailMicros = 2'000'000;
  constexpr std::int64_t firstTickMicros = 1'200'001;
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 4;
  auto *measure = new bms_parser::Measure();
  addLongNote(*measure, headMicros, tailMicros, 1,
              bms_parser::LongNoteType::HellChargeNote);
  addLongNote(*measure, headMicros, tailMicros, 2,
              bms_parser::LongNoteType::HellChargeNote);
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto gainingIds = longPairIds(definition, 1);
  const auto losingIds = longPairIds(definition, 2);
  require(gainingIds.head < losingIds.head,
          "multi-HCN fixture gives the gaining head the lower NoteId");
  const auto config = gameplay::GameplaySimulationConfig{
      .judge = gameplay::CompiledGameplayJudge::from(Judge(1)),
      .attempt = {.startingGaugePercent = 50,
                  .replayCapacity = 64,
                  .automaticResultCapacity = 64}};
  gameplay::GameplaySimulation oneShot(definition, config);
  gameplay::GameplaySimulation chunked(definition, config);
  const auto setOppositeDirections = [&](gameplay::GameplaySimulation &value) {
    value.applyPressAt(1, 1, {.songTimeMicros = headMicros});
    value.applyPressAt(2, 2, {.songTimeMicros = headMicros});
    value.applyReleaseAt(2, {.songTimeMicros = headMicros});
  };
  setOppositeDirections(oneShot);
  setOppositeDirections(chunked);
  const auto *const replayStorage = oneShot.replayEvents().data();
  const auto *const gaugeStorage = oneShot.scoreState().gaugeHistory.data();
  const auto *const balanceStorage = oneShot.hellChargeBalances().data();
  const auto *const resultStorage = oneShot.automaticResults().data();

  constexpr std::int64_t targetMicros = 1'800'001;
  const auto oneShotAdvance = oneShot.advanceTo(targetMicros, 21'000'000);
  std::vector<gameplay::GameplayReplayEvent> chunkedGaugeResults;
  for (const auto chunk : std::array<std::int64_t, 8>{
           1'000'003, 1'199'999, 1'200'001, 1'355'555,
           1'400'000, 1'600'002, 1'777'777, targetMicros}) {
    const auto result = chunked.advanceTo(chunk, 21'000'001);
    for (const auto &transaction : result.transactions) {
      require(!transaction.hasJudge && transaction.hasReplayEvent &&
                  transaction.replayEvent.action ==
                      gameplay::GameplayReplayAction::Gauge,
              "chunked HCN advances expose replay-only gauge transactions");
      chunkedGaugeResults.push_back(transaction.replayEvent);
    }
  }

  requireSameHellChargeOutcome(oneShot, chunked);
  require(oneShot.replayEvents().data() == replayStorage &&
              oneShot.scoreState().gaugeHistory.data() == gaugeStorage &&
              oneShot.hellChargeBalances().data() == balanceStorage &&
              oneShot.automaticResults().data() == resultStorage &&
              !oneShot.replayOverflowed() &&
              !oneShot.automaticResultOverflowed() &&
              !oneShot.scoreState().gaugeHistoryOverflowed(),
          "Hell Charge hot integration keeps all bounded storage fixed");
  require(oneShot.hellChargeBalances()[gainingIds.head] == 1 &&
              oneShot.hellChargeBalances()[losingIds.head] == -1,
          "opposite multi-HCN balances retain deterministic remainders");

  std::vector<gameplay::GameplayReplayEvent> gaugeEvents;
  for (const auto &event : oneShot.replayEvents()) {
    if (event.action == gameplay::GameplayReplayAction::Gauge) {
      gaugeEvents.push_back(event);
    }
  }
  require(gaugeEvents.size() == 8,
          "four shared crossing times emit two ordered gauge ticks each");
  require(oneShotAdvance.transactions.size() == gaugeEvents.size() &&
              chunkedGaugeResults == gaugeEvents,
          "one-shot and chunked advances expose the same ordered HCN results");
  for (std::size_t index = 0; index < gaugeEvents.size(); ++index) {
    require(!oneShotAdvance.transactions[index].hasJudge &&
                oneShotAdvance.transactions[index].hasReplayEvent &&
                oneShotAdvance.transactions[index].judge.judgement ==
                    gaugeEvents[index].judgement &&
                oneShotAdvance.transactions[index].replayEvent ==
                    gaugeEvents[index],
            "one-shot HCN results carry each complete ordered replay");
  }
  for (std::size_t crossing = 0; crossing < 4; ++crossing) {
    const auto expectedTime =
        firstTickMicros + static_cast<std::int64_t>(crossing) * 200'000;
    const auto &gain = gaugeEvents[crossing * 2];
    const auto &loss = gaugeEvents[crossing * 2 + 1];
    require(gain.songTimeMicros == expectedTime &&
                loss.songTimeMicros == expectedTime &&
                gain.judgeTimeMicros == expectedTime &&
                loss.judgeTimeMicros == expectedTime &&
                gain.judgement == Great && loss.judgement == Bad &&
                gain.noteId == gameplay::kInvalidNoteId &&
                loss.noteId == gameplay::kInvalidNoteId && gain.lane == -1 &&
                loss.lane == -1,
            "same-time multi-HCN ticks use ascending NoteId order");
  }
  const auto finalSnapshot = oneShot.snapshot();
  for (std::size_t index = 0; index < gaugeEvents.size(); ++index) {
    const auto &event = gaugeEvents[index];
    require(event.gaugeType == finalSnapshot.gaugeType &&
                event.combo == finalSnapshot.combo &&
                event.score == finalSnapshot.score &&
                std::bit_cast<std::uint32_t>(event.gauge) ==
                    std::bit_cast<std::uint32_t>(
                        oneShot.scoreState().gaugeHistory[index + 3]),
            "each multi-HCN replay stores its complete post-gauge state");
  }
}

void testChartCompletionAndFinalSummaryFreezeAtTrailingTimelineGrace() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 2;
  chart.Meta.KeyMode = 7;
  auto *measure = new bms_parser::Measure();
  auto *first = addTimeline(*measure, 1'000'000);
  first->SetNote(1, new bms_parser::Note(1));
  auto *second = addTimeline(*measure, 2'000'000);
  second->SetNote(2, new bms_parser::Note(2));
  addTimeline(*measure, 3'000'000);
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto compiledJudge = gameplay::CompiledGameplayJudge::from(Judge(1));
  const std::int64_t completionMicros =
      definition.metadata().finalTimelineTimeMicros +
      compiledJudge.latePoorTimingMicros() + 1;
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = compiledJudge,
       .attempt = {.initialGaugeType = GaugeType::Easy,
                   .startingGaugePercent = 100,
                   .autoPlay = true,
                   .replayCapacity = 16,
                   .automaticResultCapacity = 16,
                   .gaugeHistoryCapacity = 16}});

  const auto before = simulation.advanceTo(completionMicros - 1, 22'000'000);
  require(!simulation.terminal() &&
              simulation.terminalReason() ==
                  gameplay::GameplayTerminalReason::None &&
              simulation.noteState(definition.chronologicalNotes()[0])
                  .played &&
              simulation.noteState(definition.chronologicalNotes()[1])
                  .played &&
              before.advancedToMicros == completionMicros - 1,
          "resolved identities do not complete before empty trailing timeline "
          "grace");

  const auto completed = simulation.advanceTo(completionMicros, 22'000'001);
  const auto terminalSnapshot = simulation.terminalSnapshot();
  const auto summary = simulation.finalSummary();
  require(completed.transactions.empty() && simulation.terminal() &&
              simulation.terminalReason() ==
                  gameplay::GameplayTerminalReason::ChartComplete &&
              sameAttemptSnapshot(terminalSnapshot, simulation.snapshot()),
          "chart completion latches exactly at compiled trailing grace after "
          "all identities resolve");
  require(summary.score == 4 && summary.maxCombo == 2 &&
              summary.comboBreak == 0 && summary.totalNotes == 2 &&
              summary.finalGauge == terminalSnapshot.gauge &&
              summary.clearTypeRank == terminalSnapshot.clearTypeRank &&
              summary.fullComboAchieved,
          "parser-free final summary exposes replay and full-combo promotion "
          "inputs");

  const auto repeated =
      simulation.advanceTo(completionMicros + 1, 22'000'002);
  require(repeated.transactions.empty() &&
              repeated.advancedToMicros == completionMicros &&
              simulation.terminalReason() ==
                  gameplay::GameplayTerminalReason::ChartComplete &&
              sameAttemptSnapshot(simulation.terminalSnapshot(),
                                  terminalSnapshot) &&
              sameFinalSummary(simulation.finalSummary(), summary),
          "chart terminal snapshot and final summary are immutable");
}

void testSurvivalFailureFinishesTransactionThenFreezesEveryEntryPoint() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 1;
  auto *measure = new bms_parser::Measure();
  constexpr std::int64_t noteMicros = 1'000'000;
  auto *timeline = addTimeline(*measure, noteMicros);
  timeline->SetNote(1, new bms_parser::Note(1));
  addTimeline(*measure, 4'000'000);
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto compiledJudge = gameplay::CompiledGameplayJudge::from(Judge(1));
  const std::int64_t missMicros =
      noteMicros + compiledJudge.latePoorTimingMicros() + 1;
  gameplay::GameplaySimulation simulation(
      definition,
      {.judge = compiledJudge,
       .allowedNoteRange =
           gameplay::GameplayTimeRange{.startMicros = 0,
                                       .endMicros = 5'000'000},
       .attempt = {.initialGaugeType = GaugeType::Hard,
                   .startingGaugePercent = 1,
                   .replayCapacity = 8,
                   .automaticResultCapacity = 8,
                   .gaugeHistoryCapacity = 8}});

  const auto failed = simulation.advanceTo(missMicros, 23'000'000);
  require(failed.transactions.size() == 1 &&
              failed.transactions.front().noteId ==
                  definition.chronologicalNotes().front() &&
              failed.transactions.front().hasJudge &&
              failed.transactions.front().judge.judgement == Poor &&
              failed.transactions.front().hasReplayEvent &&
              simulation.noteState(
                  definition.chronologicalNotes().front()).played &&
              simulation.noteState(
                  definition.chronologicalNotes().front()).dead &&
              simulation.snapshot().judgeCounts[Poor] == 1 &&
              simulation.snapshot().gauge == 0.0F &&
              simulation.replayEvents().size() == 1 &&
              simulation.automaticResults().size() == 1 &&
              simulation.scoreState().gaugeHistory.size() == 1,
          "survival failure transaction finishes note, score, gauge, replay, "
          "and result mutation");
  require(simulation.terminalReason() ==
                  gameplay::GameplayTerminalReason::SurvivalGaugeFailed &&
              sameAttemptSnapshot(simulation.terminalSnapshot(),
                                  simulation.snapshot()),
          "survival failure latches at the completed transaction boundary");

  const auto frozenSnapshot = simulation.snapshot();
  const auto frozenTerminalSnapshot = simulation.terminalSnapshot();
  const auto frozenSummary = simulation.finalSummary();
  const auto frozenNote =
      simulation.noteState(definition.chronologicalNotes().front());
  const std::vector<gameplay::GameplayReplayEvent> frozenReplays(
      simulation.replayEvents().begin(), simulation.replayEvents().end());
  const std::vector<float> frozenGaugeHistory(
      simulation.scoreState().gaugeHistory.begin(),
      simulation.scoreState().gaugeHistory.end());
  const auto *const frozenResultStorage = simulation.automaticResults().data();
  const auto frozenResultSize = simulation.automaticResults().size();
  const auto frozenSearch = simulation.lastSearchStats();
  const auto frozenAdvance = simulation.lastAdvanceStats();

  const auto directPress =
      simulation.pressLane(2, {.songTimeMicros = missMicros + 1});
  const auto directRelease =
      simulation.releaseLane(1, {.songTimeMicros = missMicros + 2});
  const auto wrappedPress = simulation.applyPressAt(
      2, 2, {.songTimeMicros = missMicros + 3});
  const auto wrappedRelease = simulation.applyReleaseAt(
      1, {.songTimeMicros = missMicros + 4});
  const auto laterAdvance = simulation.advanceTo(missMicros + 5, 23'000'001);
  const auto laterFinalize =
      simulation.finalizePracticeRange(missMicros + 6, 23'000'002);

  require(!directPress.hasReplayEvent && !directRelease.hasReplayEvent &&
              !wrappedPress.hasReplayEvent && !wrappedRelease.hasReplayEvent &&
              laterAdvance.transactions.empty() &&
              laterFinalize.transactions.empty() &&
              laterAdvance.advancedToMicros == missMicros &&
              laterFinalize.advancedToMicros == missMicros,
          "all low-level, wrapper, advance, and finalization entry points are "
          "empty after terminal");
  const auto &noteAfter =
      simulation.noteState(definition.chronologicalNotes().front());
  require(noteAfter.played == frozenNote.played &&
              noteAfter.dead == frozenNote.dead &&
              noteAfter.holding == frozenNote.holding &&
              noteAfter.playedTimeMicros == frozenNote.playedTimeMicros &&
              noteAfter.releaseTimeMicros == frozenNote.releaseTimeMicros &&
              !simulation.lanePressed(1) && !simulation.lanePressed(2) &&
              sameAttemptSnapshot(simulation.snapshot(), frozenSnapshot) &&
              sameAttemptSnapshot(simulation.terminalSnapshot(),
                                  frozenTerminalSnapshot) &&
              sameFinalSummary(simulation.finalSummary(), frozenSummary) &&
              std::ranges::equal(simulation.replayEvents(), frozenReplays) &&
              simulation.scoreState().gaugeHistory == frozenGaugeHistory &&
              simulation.automaticResults().data() == frozenResultStorage &&
              simulation.automaticResults().size() == frozenResultSize &&
              simulation.lastSearchStats().notesExamined ==
                  frozenSearch.notesExamined &&
              simulation.lastAdvanceStats().notesExamined ==
                  frozenAdvance.notesExamined,
          "terminal state cannot be mutated by any later entry point");
}

void testCompleteOutcomeMatchesForLargeAndChunkedScheduling() {
  bms_parser::Chart chart;
  chart.Meta.TotalNotes = 4;
  auto *measure = new bms_parser::Measure();
  addLongNote(*measure, 1'000'000, 1'700'000, 1,
              bms_parser::LongNoteType::HellChargeNote);
  auto *firstNormal = addTimeline(*measure, 1'200'000);
  firstNormal->SetNote(2, new bms_parser::Note(2));
  auto *secondNormal = addTimeline(*measure, 2'000'000);
  secondNormal->SetNote(3, new bms_parser::Note(3));
  addTimeline(*measure, 2'400'000);
  chart.Measures.push_back(measure);

  const auto definition = gameplay::buildGameplayDefinition(chart, 0);
  const auto compiledJudge = gameplay::CompiledGameplayJudge::from(Judge(1));
  const auto config = gameplay::GameplaySimulationConfig{
      .judge = compiledJudge,
      .attempt = {.startingGaugePercent = 80,
                  .replayCapacity = 64,
                  .automaticResultCapacity = 64,
                  .gaugeHistoryCapacity = 64}};
  gameplay::GameplaySimulation oneShot(definition, config);
  gameplay::GameplaySimulation chunked(definition, config);
  const auto applyIdenticalInputs = [](gameplay::GameplaySimulation &value) {
    value.applyPressAt(1, 1, {.songTimeMicros = 1'000'000});
    value.applyReleaseAt(1, {.songTimeMicros = 1'050'000});
  };
  applyIdenticalInputs(oneShot);
  applyIdenticalInputs(chunked);

  const std::int64_t completionMicros =
      definition.metadata().finalTimelineTimeMicros +
      compiledJudge.latePoorTimingMicros() + 1;
  oneShot.advanceTo(completionMicros, 24'000'000);
  for (const std::int64_t chunk : std::array<std::int64_t, 7>{
           1'100'000, 1'200'001, 1'450'000, 1'700'001,
           2'000'000, 2'300'000, completionMicros}) {
    chunked.advanceTo(chunk, 24'000'000);
  }

  require(oneShot.terminalReason() ==
                  gameplay::GameplayTerminalReason::ChartComplete &&
              chunked.terminalReason() ==
                  gameplay::GameplayTerminalReason::ChartComplete,
          "large and chunked schedules both reach chart completion");
  requireSameCompleteOutcome(definition, oneShot, chunked);
}
} // namespace

int main() {
  testDefinitionCompilesAutomaticMetadata();
  testDefinitionUsesDefaultGaugeTotalWhenChartOmitsTotal();
  testDefinitionCompilesChronologicalHellChargeHeads();
  testHellChargeStrictThresholdsAndSerialInputState();
  testMultipleHellChargeCrossingsUseTimeThenNoteIdOrder();
  testAttemptInitializesConfiguredAndCarriedState();
  testCapacityFaultsLatchIndependentlyWithoutGrowth();
  testSimultaneousFaultPrecedenceAndFirstReasonImmutability();
  testNormalLatePoorUsesStrictDeadlineAndIsIdempotent();
  testLandmineDetonatesOrExpiresFromPriorLaneState();
  testSameTimeAutomaticWorkPrecedesPressAndRelease();
  testEqualDeadlineUsesAtTimingPhaseBeforeLatePoor();
  testAutoplayNormalEmitsOnePressReplayAndVisualRelease();
  testAutomaticResultCapacityLatchesWithoutGrowth();
  testGlobalAutomaticCursorsDoNotReexamineCompletedIdentities();
  testBackwardAdvanceIsIgnoredWithoutRollingBackTime();
  testUnpressedLongNoteDeadlineIdentityMatrix();
  testHeldLongNoteAutomaticReleaseMatrix();
  testAutoplayChargeAndHellChargeReleaseAtTiming();
  testStartInitializationResolvesPreStartWorkWithoutTransactions();
  testPracticeFinalizationMatchesHalfOpenIdentityRules();
  testChartCompletionAndFinalSummaryFreezeAtTrailingTimelineGrace();
  testSurvivalFailureFinishesTransactionThenFreezesEveryEntryPoint();
  testCompleteOutcomeMatchesForLargeAndChunkedScheduling();
  return 0;
}
