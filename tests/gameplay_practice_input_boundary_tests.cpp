#include "scene/play/BMSRenderer.h"
#include "scene/play/NoteTimeRange.h"
#include "scene/play/PracticeNoteFinalizer.h"
#include "scene/play/RhythmLaneInputController.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <vector>

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

bms_parser::Note *addNote(bms_parser::Measure &measure, long long timingMicros,
                          int lane) {
  auto *timeline = addTimeline(measure, timingMicros);
  auto *note = new bms_parser::Note(1);
  timeline->SetNote(lane, note);
  return note;
}

bms_parser::LongNote *addLongNote(bms_parser::Measure &measure,
                                  long long headMicros, long long tailMicros,
                                  int lane, bms_parser::LongNoteType type) {
  auto *headTimeline = addTimeline(measure, headMicros);
  auto *tailTimeline = addTimeline(measure, tailMicros);
  auto *head = new bms_parser::LongNote(1, type);
  auto *tail = new bms_parser::LongNote(1, type);
  head->Tail = tail;
  tail->Head = head;
  headTimeline->SetNote(lane, head);
  tailTimeline->SetNote(lane, tail);
  return head;
}

void testActualInputAndJudgeRange() {
  constexpr long long startMicros = 500'000;
  constexpr long long endMicros = 1'000'000;
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *lastValid = addNote(*measure, endMicros - 1, 1);
  auto *atEnd = addNote(*measure, endMicros, 2);
  chart.Measures.push_back(measure);

  std::unordered_map<int, bool> lanePressed{{1, false}, {2, false}};
  RhythmLaneInputController controller(
      &chart, nullptr, lanePressed, CourseJudgementConstraint::None, 0,
      NoteTimeRange{.startMicros = startMicros, .endMicros = endMicros});
  lanePressed[1] = false;
  lanePressed[2] = false;

  const auto earlyBoundaryAttempt = controller.pressLane(
      2, {.songTimeMicros = endMicros - 100'000,
          .laneBeamTimeMicros = 1,
          .notePriorityMode = AppSettings::NotePriorityMode::Lowest});
  require(earlyBoundaryAttempt.note == nullptr && !atEnd->IsPlayed,
          "an early input cannot select the note exactly at end");

  lanePressed[2] = false;
  const auto lastValidHit = controller.pressLane(
      1, {.songTimeMicros = endMicros - 1, .laneBeamTimeMicros = 2});
  require(lastValidHit.note == lastValid && lastValid->IsPlayed,
          "the note immediately before end remains hittable");

  const std::vector<std::pair<long long, long long>> offsetClocks = {
      {900'000, 100'000},
      {1'100'000, -100'000},
  };
  for (const auto [rawSongTimeMicros, audioOffsetMicros] : offsetClocks) {
    lanePressed[1] = false;
    const auto eventBeforeUpdate = controller.pressLane(
        1, {.songTimeMicros = rawSongTimeMicros + audioOffsetMicros,
            .laneBeamTimeMicros = 3,
            .inputDelay = 0.2});
    require(eventBeforeUpdate.note == nullptr &&
                !eventBeforeUpdate.hasReplayEvent && !lanePressed[1],
            "offset-adjusted boundary input cannot mutate before update");
  }

  lanePressed[1] = true;
  const auto releaseBeforeUpdate = controller.releaseLane(
      1, {.songTimeMicros = endMicros,
          .laneBeamTimeMicros = 4,
          .inputDelay = 0.2});
  require(releaseBeforeUpdate.note == nullptr &&
              !releaseBeforeUpdate.hasReplayEvent && lanePressed[1],
          "boundary release is blocked before lane mutation");

  atEnd->Reset();
  std::unordered_map<int, bool> unrestrictedPressed{{2, false}};
  RhythmLaneInputController unrestricted(&chart, nullptr, unrestrictedPressed);
  unrestrictedPressed[2] = false;
  const auto ordinaryHit = unrestricted.pressLane(
      2, {.songTimeMicros = endMicros, .laneBeamTimeMicros = 5});
  require(ordinaryHit.note == atEnd && atEnd->IsPlayed,
          "ordinary gameplay remains unrestricted");
}

void testExactPendingNoteFinalization() {
  constexpr long long startMicros = 500'000;
  constexpr long long endMicros = 1'000'000;
  const NoteTimeRange range{.startMicros = startMicros,
                            .endMicros = endMicros};
  bms_parser::Chart chart;
  auto *measure = new bms_parser::Measure();
  auto *lastValid = addNote(*measure, endMicros - 1, 1);
  auto *atEnd = addNote(*measure, endMicros, 2);
  auto *afterEnd = addNote(*measure, endMicros + 1, 3);
  auto *chargeHead =
      addLongNote(*measure, endMicros - 20, endMicros - 10, 4,
                  bms_parser::LongNoteType::ChargeNote);
  auto *classicHead =
      addLongNote(*measure, endMicros - 30, endMicros + 10, 5,
                  bms_parser::LongNoteType::LongNote);
  auto *heldClassicHead =
      addLongNote(*measure, endMicros - 40, endMicros + 20, 6,
                  bms_parser::LongNoteType::LongNote);
  heldClassicHead->Press(endMicros - 40);
  auto *mineTimeline = addTimeline(*measure, endMicros - 2);
  auto *mine = new bms_parser::LandmineNote(5.0f);
  mineTimeline->SetLandmineNote(7, mine);
  chart.Measures.push_back(measure);

  const auto misses = finalizePendingPracticeNotes(
      chart, range, endMicros - 1, 0);
  require(misses.size() == 5,
          "normal, charge identities, and classic heads finalize once");
  require(lastValid->IsPlayed && lastValid->IsDead,
          "end-1 pending note becomes a miss");
  require(chargeHead->IsPlayed && chargeHead->IsDead &&
              chargeHead->Tail->IsPlayed && chargeHead->Tail->IsDead,
          "in-range charge identities each finalize as misses");
  require(classicHead->IsPlayed && classicHead->IsDead &&
              !classicHead->Tail->IsPlayed,
          "classic head finalizes without touching its out-of-range tail");
  require(heldClassicHead->IsPlayed && heldClassicHead->IsDead &&
              !heldClassicHead->Tail->IsPlayed,
          "held classic head receives its pending judgement at the boundary");
  require(!atEnd->IsPlayed && !afterEnd->IsPlayed && !mine->IsPlayed &&
              !mine->IsDead,
          "at/after notes and mines are never finalized as misses");

  const auto duplicate = finalizePendingPracticeNotes(
      chart, range, endMicros - 1, 0);
  require(duplicate.empty(), "practice finalization is idempotent");
}

} // namespace

int main() {
  testActualInputAndJudgeRange();
  testExactPendingNoteFinalization();
  return 0;
}
