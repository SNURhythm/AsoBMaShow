#include "scene/play/GameplayDefinition.h"
#include "scene/play/GameplayScoreState.h"
#include "scene/play/GameplaySimulation.h"
#include "scene/play/Judge.h"

#include "bms_parser.hpp"

#include <algorithm>
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
} // namespace

int main() {
  testDefinitionCompilesAutomaticMetadata();
  testDefinitionUsesDefaultGaugeTotalWhenChartOmitsTotal();
  testDefinitionCompilesChronologicalHellChargeHeads();
  testAttemptInitializesConfiguredAndCarriedState();
  testReplayAndGaugeHistoryCapacityLatchWithoutGrowth();
  return 0;
}
