#include "scene/play/GameplayDefinition.h"
#include "scene/play/GameplayScoreState.h"

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
} // namespace

int main() {
  testDefinitionCompilesAutomaticMetadata();
  testDefinitionUsesDefaultGaugeTotalWhenChartOmitsTotal();
  testDefinitionCompilesChronologicalHellChargeHeads();
  return 0;
}
