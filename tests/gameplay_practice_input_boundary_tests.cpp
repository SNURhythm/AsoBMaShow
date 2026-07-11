#include "scene/play/BMSRenderer.h"
#include "scene/play/GamePlayStartOptions.h"
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

Judgement pressWithJudge(Judge effectiveJudge, long long diffMicros) {
  constexpr long long noteMicros = 1'000'000;
  bms_parser::Chart chart;
  chart.Meta.Rank = 1;
  auto *measure = new bms_parser::Measure();
  auto *note = addNote(*measure, noteMicros, 1);
  chart.Measures.push_back(measure);
  std::unordered_map<int, bool> lanePressed{{1, false}};
  RhythmLaneInputController controller(&chart, nullptr, lanePressed,
                                       std::move(effectiveJudge));
  lanePressed[1] = false;
  const auto result = controller.pressLane(
      1, {.songTimeMicros = noteMicros + diffMicros,
          .laneBeamTimeMicros = noteMicros + diffMicros});
  require(result.note == note, "resolved Judge selects the expected note");
  return result.judge.judgement;
}

Judgement releaseWithJudge(Judge effectiveJudge, long long diffMicros) {
  constexpr long long headMicros = 900'000;
  constexpr long long tailMicros = 1'000'000;
  bms_parser::Chart chart;
  chart.Meta.Rank = 1;
  auto *measure = new bms_parser::Measure();
  auto *head = addLongNote(*measure, headMicros, tailMicros, 1,
                           bms_parser::LongNoteType::ChargeNote);
  chart.Measures.push_back(measure);
  head->Press(headMicros);
  std::unordered_map<int, bool> lanePressed{{1, true}};
  RhythmLaneInputController controller(&chart, nullptr, lanePressed,
                                       std::move(effectiveJudge));
  lanePressed[1] = true;
  const auto result = controller.releaseLane(
      1, {.songTimeMicros = tailMicros + diffMicros,
          .laneBeamTimeMicros = tailMicros + diffMicros});
  require(result.note == head->Tail,
          "resolved Judge selects the expected long-note tail");
  return result.judge.judgement;
}

void testControllerUsesResolvedEffectiveJudge() {
  StartOptions identityOptions;
  require(pressWithJudge(makeEffectiveJudgeAtPlayStart(identityOptions, 1),
                         20'000) == Great,
          "100/100 live input preserves rank windows");
  require(releaseWithJudge(makeEffectiveJudgeAtPlayStart(identityOptions, 1),
                           20'000) == Great,
          "100/100 live release preserves rank windows");

  StartOptions halfRate;
  halfRate.playback.percent = 50;
  require(pressWithJudge(makeEffectiveJudgeAtPlayStart(halfRate, 1), 20'000) ==
              Good,
          "50 percent playback narrows live input windows");
  require(releaseWithJudge(makeEffectiveJudgeAtPlayStart(halfRate, 1),
                           20'000) == Good,
          "50 percent playback narrows live release windows");

  StartOptions doubleRate;
  doubleRate.playback.percent = 200;
  require(pressWithJudge(makeEffectiveJudgeAtPlayStart(doubleRate, 1),
                         50'000) == Great,
          "200 percent playback widens live input windows");

  StartOptions halfJudgeScale;
  halfJudgeScale.judgeWindowScalePercent = 50;
  require(pressWithJudge(makeEffectiveJudgeAtPlayStart(halfJudgeScale, 1),
                         20'000) == Good,
          "practice judge scale affects live input");

  StartOptions compensatingScale;
  compensatingScale.playback.percent = 50;
  compensatingScale.judgeWindowScalePercent = 200;
  require(pressWithJudge(makeEffectiveJudgeAtPlayStart(compensatingScale, 1),
                         20'000) == Great,
          "50 playback and 200 judge scale compose to identity");

  StartOptions courseConstrained;
  courseConstrained.courseConstraints.judgement =
      CourseJudgementConstraint::NoGood;
  require(pressWithJudge(
              makeEffectiveJudgeAtPlayStart(courseConstrained, 1), 50'000) ==
              Bad,
          "course judgement constraint affects live input");

  bms_parser::ChartMeta replayMeta;
  replayMeta.Rank = 1;
  replayMeta.MD5 = "replay-window-md5";
  replayMeta.SHA256 = std::string(64, 'a');
  StartOptions replayOptions;
  replayOptions.replayJudgeOverride = ScoreStageProvenance{
      .chartMd5 = replayMeta.MD5,
      .chartSha256 = replayMeta.SHA256,
      .effectiveJudgeWindows = {
          {PGreat, -123, 123},
          {Great, -456, 456},
          {Good, -789, 789},
          {Bad, -1'000, 1'000},
          {Kpoor, -1'200, 1'200},
      },
  };
  const Judge replayJudge =
      makeEffectiveJudgeAtPlayStart(replayOptions, replayMeta);
  require(pressWithJudge(replayJudge, 123) == PGreat,
          "replay exact window includes its boundary");
  require(pressWithJudge(replayJudge, 124) == Great,
          "replay exact windows replace rank windows for live input");
  require(releaseWithJudge(replayJudge, 124) == Great,
          "replay exact windows apply to live release judgement");
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
      &chart, nullptr, lanePressed, Judge(chart.Meta.Rank), 0,
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
  RhythmLaneInputController unrestricted(
      &chart, nullptr, unrestrictedPressed, Judge(chart.Meta.Rank));
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
  testControllerUsesResolvedEffectiveJudge();
  testActualInputAndJudgeRange();
  testExactPendingNoteFinalization();
  return 0;
}
