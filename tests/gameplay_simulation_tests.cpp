#include "scene/play/CompiledGameplayJudge.h"
#include "scene/play/GameplayDefinition.h"
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
} // namespace

int main() {
  testCompiledJudgePreservesResolvedWindows();
  testDefinitionUsesStableIdsAndLaneIndices();
  return 0;
}
