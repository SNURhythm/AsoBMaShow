#include "RhythmLaneInputController.h"
#include "BMSRenderer.h"
#include "GameplayNoteJudgeRole.h"
#include "ManualKeysoundSelection.h"
#include "../../CoursePlaySession.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <span>
#include <utility>

namespace {
struct PressLaneCandidate {
  int lane = 0;
  bms_parser::Note *note = nullptr;
  JudgeResult judge = JudgeResult(None, 0);
};

long long noteTimingMicros(const bms_parser::Note *note);

std::size_t inputCandidateCapacity(const bms_parser::Chart *chart) {
  std::size_t count = 1;
  if (chart == nullptr) {
    return count;
  }
  for (const auto *measure : chart->Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      count += static_cast<std::size_t>(std::ranges::count_if(
          timeline->Notes, [](bms_parser::Note *note) {
            return note != nullptr && !note->IsLandmineNote();
          }));
    }
  }
  return count;
}

JudgeResult normalizeLongNoteReleaseJudge(const JudgeResult &judgeResult) {
  if (judgeResult.judgement == None || judgeResult.judgement == Kpoor ||
      judgeResult.judgement == Poor) {
    return JudgeResult(Bad, judgeResult.Diff);
  }
  return judgeResult;
}

int longNoteJudgeSeverity(Judgement judgement) noexcept {
  switch (judgement) {
  case PGreat:
    return 0;
  case Great:
    return 1;
  case Good:
    return 2;
  case Bad:
    return 3;
  case Kpoor:
  case Poor:
    return 4;
  case None:
  case JudgementCount:
    return 5;
  }
  return 5;
}

JudgeResult worseLongNoteJudge(const JudgeResult &head,
                               const JudgeResult &tail) noexcept {
  const int headSeverity = longNoteJudgeSeverity(head.judgement);
  const int tailSeverity = longNoteJudgeSeverity(tail.judgement);
  if (tailSeverity != headSeverity) {
    return tailSeverity > headSeverity ? tail : head;
  }
  return std::llabs(tail.Diff) > std::llabs(head.Diff) ? tail : head;
}

JudgeResult judgeClassicLongNoteRelease(
    const gameplay::CompiledGameplayJudge &judge,
    const bms_parser::ChartMeta &chartMeta, int longNoteModeOverride,
    bms_parser::LongNote *tail, long long releasedTime,
    const JudgeResult &acceptedHeadJudge) {
  if (tail == nullptr || !tail->IsTail() || tail->Head == nullptr) {
    return JudgeResult(None, 0);
  }

  if (judge.rules().ruleset == GameplayRuleset::LR2) {
    const JudgeResult head =
        normalizeLongNoteReleaseJudge(acceptedHeadJudge);
    const long long tailDiff = releasedTime - noteTimingMicros(tail);
    if (std::llabs(tailDiff) <= 120'000) {
      return head;
    }
    return worseLongNoteJudge(head, JudgeResult(Bad, tailDiff));
  }

  const JudgeResult headJudge = judge.judgeAt(
      gameplay::judgeRoleFor(tail->Head, chartMeta, longNoteModeOverride),
      noteTimingMicros(tail->Head), tail->Head->PlayedTime);
  const JudgeResult tailJudge = judge.judgeAt(
      gameplay::judgeRoleFor(tail, chartMeta, longNoteModeOverride),
      noteTimingMicros(tail), releasedTime);
  const auto absDiff = [](long long value) {
    return value < 0 ? -value : value;
  };
  return normalizeLongNoteReleaseJudge(
      absDiff(tailJudge.Diff) > absDiff(headJudge.Diff) ? tailJudge
                                                        : headJudge);
}

long long noteTimingMicros(const bms_parser::Note *note) {
  return note != nullptr && note->Timeline != nullptr ? note->Timeline->Timing
                                                      : 0;
}

long long absoluteTimeDistance(long long a, long long b) {
  return a > b ? a - b : b - a;
}

bool preferByTimingWindow(const PressLaneCandidate &current,
                          const PressLaneCandidate &next,
                          long long inputTime,
                          const gameplay::CompiledGameplayJudge &judge,
                          Judgement threshold) {
  if (next.note == nullptr || next.note->IsPlayed) {
    return false;
  }
  const auto window = judge.window(threshold);
  if (!window.has_value()) {
    return false;
  }

  const long long currentTiming = noteTimingMicros(current.note);
  const long long nextTiming = noteTimingMicros(next.note);
  return currentTiming < inputTime - window->lateMicros &&
         nextTiming <= inputTime - window->earlyMicros;
}

bool shouldPreferCandidate(const PressLaneCandidate &current,
                           const PressLaneCandidate &next,
                           long long inputTime,
                           const gameplay::CompiledGameplayJudge &judge,
                           AppSettings::NotePriorityMode mode) {
  switch (mode) {
  case AppSettings::NotePriorityMode::Combo:
    return preferByTimingWindow(current, next, inputTime, judge, Good);
  case AppSettings::NotePriorityMode::Duration:
    return next.note != nullptr && !next.note->IsPlayed &&
           absoluteTimeDistance(noteTimingMicros(current.note), inputTime) >
               absoluteTimeDistance(noteTimingMicros(next.note), inputTime);
  case AppSettings::NotePriorityMode::Score:
    return preferByTimingWindow(current, next, inputTime, judge, Great);
  case AppSettings::NotePriorityMode::Lowest:
    return false;
  }
  return false;
}

void setReplayEvent(RhythmLaneInputController::Result &result,
                    ReplayEventAction action, int lane,
                    const bms_parser::Note *note, long long songTimeMicros,
                    long long judgeTimeMicros,
                    const JudgeResult &judgeResult) {
  result.hasReplayEvent = true;
  result.replayEvent = {action, lane, note, songTimeMicros, judgeTimeMicros,
                        judgeResult};
}
} // namespace

RhythmLaneInputController::RhythmLaneInputController(
    bms_parser::Chart *chart, BMSRenderer *renderer,
    std::unordered_map<int, bool> &lanePressed, Judge effectiveJudge,
    int longNoteModeOverride,
    std::optional<NoteTimeRange> allowedNoteRange)
    : RhythmLaneInputController(
          chart, renderer, lanePressed,
          gameplay::CompiledGameplayJudge::from(effectiveJudge),
          longNoteModeOverride, std::move(allowedNoteRange)) {}

RhythmLaneInputController::RhythmLaneInputController(
    bms_parser::Chart *chart, BMSRenderer *renderer,
    std::unordered_map<int, bool> &lanePressed,
    gameplay::CompiledGameplayJudge effectiveJudge, int longNoteModeOverride,
    std::optional<NoteTimeRange> allowedNoteRange)
    : chart(chart), renderer(renderer), lanePressed(lanePressed),
      longNoteModeOverride(longNoteModeOverride),
      judge(std::move(effectiveJudge)),
      allowedNoteRange(std::move(allowedNoteRange)) {
  latePoorTiming = judge.automaticPoorLateMicros();
  const std::size_t capacity = inputCandidateCapacity(chart);
  inputTransactions.reserve(capacity);
  judgeCandidateNotes.reserve(capacity);
  judgeCandidates.reserve(capacity);
  multiBadSourceIndices.reserve(capacity);
  acceptedLongHeadJudges.reserve(capacity);
  indexKeysoundNotes();
  resetLaneStates();
}

void RhythmLaneInputController::indexKeysoundNotes() {
  keysoundNotesByLane.clear();
  if (chart == nullptr) {
    return;
  }
  for (const auto *measure : chart->Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      for (auto *note : timeline->Notes) {
        if (note == nullptr || note->IsLandmineNote()) {
          continue;
        }
        const auto *longNote =
            dynamic_cast<const bms_parser::LongNote *>(note);
        if (longNote != nullptr && longNote->IsTail()) {
          continue;
        }
        keysoundNotesByLane[note->Lane].push_back(note);
      }
      for (auto *note : timeline->InvisibleNotes) {
        if (note != nullptr) {
          keysoundNotesByLane[note->Lane].push_back(note);
        }
      }
    }
  }
  for (auto &[lane, notes] : keysoundNotesByLane) {
    (void)lane;
    std::stable_sort(notes.begin(), notes.end(),
                     [](const bms_parser::Note *left,
                        const bms_parser::Note *right) {
                       return noteTimingMicros(left) <
                              noteTimingMicros(right);
                     });
  }
}

bms_parser::Note *RhythmLaneInputController::selectFallbackKeysound(
    int mainLane, int compensateLane, long long inputTime) const {
  const auto notesFor = [&](int lane, bool available) {
    if (!available) {
      return std::span<bms_parser::Note *const>();
    }
    const auto found = keysoundNotesByLane.find(lane);
    return found == keysoundNotesByLane.end()
               ? std::span<bms_parser::Note *const>()
               : std::span<bms_parser::Note *const>(found->second);
  };
  const auto mainState = lanePressed.find(mainLane);
  const auto compensationState = lanePressed.find(compensateLane);
  const auto mainNotes = notesFor(
      mainLane, mainState != lanePressed.end() && !mainState->second);
  const auto compensationNotes = notesFor(
      compensateLane,
      compensateLane != mainLane && compensationState != lanePressed.end() &&
          !compensationState->second);
  const long long rangeStart =
      allowedNoteRange.has_value()
          ? allowedNoteRange->startMicros
          : std::numeric_limits<long long>::min();
  const long long rangeEnd =
      allowedNoteRange.has_value()
          ? allowedNoteRange->endMicros
          : std::numeric_limits<long long>::max();
  const auto selected = gameplay::selectManualKeysound<bms_parser::Note *>(
      mainNotes, compensationNotes, inputTime, rangeStart, rangeEnd,
      [](const bms_parser::Note *note) {
        return noteTimingMicros(note);
      });
  switch (selected.lane) {
  case gameplay::ManualKeysoundLane::Main:
    return mainNotes[selected.index];
  case gameplay::ManualKeysoundLane::Compensation:
    return compensationNotes[selected.index];
  case gameplay::ManualKeysoundLane::None:
    return nullptr;
  }
  return nullptr;
}

RhythmLaneInputController::ResultBatch
RhythmLaneInputController::pressLane(int lane, const InputContext &context) {
  return pressLane(lane, lane, context);
}

RhythmLaneInputController::Result
RhythmLaneInputController::pressLaneForPreparation(
    int mainLane, int compensateLane, const InputContext &context) {
  Result result;
  if (chart == nullptr ||
      (allowedNoteRange.has_value() &&
       context.songTimeMicros >= allowedNoteRange->endMicros)) {
    return result;
  }
  auto mainState = lanePressed.find(mainLane);
  const auto compensationState = lanePressed.find(compensateLane);
  if ((mainState == lanePressed.end() || mainState->second) &&
      (compensateLane == mainLane ||
       compensationState == lanePressed.end() ||
       compensationState->second)) {
    return result;
  }

  const long long eventTime = inputTimeMicros(context);
  result.keySoundNote =
      selectFallbackKeysound(mainLane, compensateLane, eventTime);
  if (mainState != lanePressed.end()) {
    mainState->second = true;
  }
  if (renderer != nullptr) {
    renderer->onLanePressed(mainLane, JudgeResult(None, 0),
                            context.laneBeamTimeMicros);
  }
  setReplayEvent(result, ReplayEventAction::Press, mainLane, nullptr,
                 eventTime, eventTime, JudgeResult(None, 0));
  return result;
}

RhythmLaneInputController::Result
RhythmLaneInputController::releaseLaneForPreparation(
    int lane, const InputContext &context) {
  Result result;
  if (chart == nullptr ||
      (allowedNoteRange.has_value() &&
       context.songTimeMicros >= allowedNoteRange->endMicros)) {
    return result;
  }
  auto laneState = lanePressed.find(lane);
  if (laneState == lanePressed.end() || !laneState->second) {
    return result;
  }
  laneState->second = false;
  if (renderer != nullptr) {
    renderer->onLaneReleased(lane, context.laneBeamTimeMicros);
  }
  const long long eventTime = inputTimeMicros(context);
  setReplayEvent(result, ReplayEventAction::Release, lane, nullptr,
                 eventTime, eventTime, JudgeResult(None, 0));
  return result;
}

RhythmLaneInputController::ResultBatch RhythmLaneInputController::pressLane(
    int mainLane, int compensateLane, const InputContext &context) {
  inputTransactions.clear();
  judgeCandidateNotes.clear();
  judgeCandidates.clear();
  multiBadSourceIndices.clear();
  Result result;
  if (chart == nullptr ||
      (allowedNoteRange.has_value() &&
       context.songTimeMicros >= allowedNoteRange->endMicros)) {
    return resultBatch(result);
  }

  auto mainLaneIt = lanePressed.find(mainLane);
  std::array<int, 2> candidates{};
  size_t candidateCount = 0;
  if (mainLaneIt != lanePressed.end() && !mainLaneIt->second) {
    candidates[candidateCount++] = mainLane;
  }
  auto compensateLaneIt = lanePressed.find(compensateLane);
  if (compensateLane != mainLane && compensateLaneIt != lanePressed.end() &&
      !compensateLaneIt->second) {
    candidates[candidateCount++] = compensateLane;
  }
  if (candidateCount == 0) {
    return resultBatch(result);
  }

  const long long inputTime = inputTimeMicros(context);
  const long long futureCutoff = judge.latestHittableNoteTiming(
      gameplay::NoteJudgeRole::Normal, inputTime);
  const bool lr2Selection =
      judge.rules().candidateSelection ==
      gameplay::CandidateSelectionMode::LR2;
  bool hasSelectedCandidate = false;
  bool stopScanning = false;
  PressLaneCandidate selectedCandidate;

  for (const auto *measure : chart->Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      if (timeline->Timing < inputTime - latePoorTiming) {
        continue;
      }
      if (timeline->Timing > futureCutoff) {
        stopScanning = true;
        break;
      }
      for (size_t candidateIdx = 0; candidateIdx < candidateCount;
           ++candidateIdx) {
        const int lane = candidates[candidateIdx];
        if (lane < 0 || static_cast<size_t>(lane) >= timeline->Notes.size()) {
          continue;
        }
        auto *note = timeline->Notes[lane];
        const auto *longNote =
            dynamic_cast<const bms_parser::LongNote *>(note);
        if (note == nullptr || note->IsPlayed || note->IsLandmineNote() ||
            (longNote != nullptr && longNote->IsTail()) ||
            !noteAllowed(note)) {
          continue;
        }
        const JudgeResult noteJudge = judge.judgeAt(
            gameplay::judgeRoleFor(note, chart->Meta, longNoteModeOverride),
            noteTimingMicros(note), inputTime);
        if (lr2Selection) {
          const std::size_t sourceIndex = judgeCandidateNotes.size();
          judgeCandidateNotes.push_back(note);
          judgeCandidates.push_back({
              .sourceIndex = sourceIndex,
              .timingMicros = noteTimingMicros(note),
              .longNoteHead = longNote != nullptr && !longNote->IsTail(),
              .judge = noteJudge,
          });
          continue;
        }
        if (noteJudge.judgement == None) {
          continue;
        }
        const PressLaneCandidate candidate{lane, note, noteJudge};
        if (!hasSelectedCandidate ||
            shouldPreferCandidate(selectedCandidate, candidate, inputTime,
                                  judge, context.notePriorityMode)) {
          selectedCandidate = candidate;
          hasSelectedCandidate = true;
        }
        if (context.notePriorityMode == AppSettings::NotePriorityMode::Lowest) {
          stopScanning = true;
          break;
        }
      }
      if (stopScanning) {
        break;
      }
    }
    if (stopScanning) {
      break;
    }
  }

  if (lr2Selection) {
    multiBadSourceIndices.resize(judgeCandidates.size());
    const auto resolution = gameplay::resolveLr2Candidates(
        judgeCandidates, multiBadSourceIndices);
    multiBadSourceIndices.resize(resolution.multiBadCount);
    if (resolution.selectedSourceIndex.has_value() &&
        *resolution.selectedSourceIndex < judgeCandidateNotes.size()) {
      const std::size_t selectedIndex = *resolution.selectedSourceIndex;
      selectedCandidate = {
          .lane = judgeCandidateNotes[selectedIndex]->Lane,
          .note = judgeCandidateNotes[selectedIndex],
          .judge = judgeCandidates[selectedIndex].judge,
      };
      hasSelectedCandidate = true;
    }
  }

  if (hasSelectedCandidate) {
    for (const std::size_t sourceIndex : multiBadSourceIndices) {
      if (sourceIndex >= judgeCandidateNotes.size()) {
        continue;
      }
      auto *multiBadNote = judgeCandidateNotes[sourceIndex];
      if (multiBadNote == nullptr || multiBadNote->IsPlayed) {
        continue;
      }
      multiBadNote->Play(inputTime);
      Result multiBad;
      multiBad.note = multiBadNote;
      multiBad.hasJudge = true;
      multiBad.judge =
          JudgeResult(Bad, inputTime - noteTimingMicros(multiBadNote));
      setReplayEvent(multiBad, ReplayEventAction::Press,
                     multiBadNote->Lane, multiBadNote, inputTime, inputTime,
                     multiBad.judge);
      inputTransactions.push_back(multiBad);
    }
    result = pressNote(selectedCandidate.note, inputTime, inputTime);
    result.note = selectedCandidate.note;
    if (auto pressedIt = lanePressed.find(selectedCandidate.lane);
        pressedIt != lanePressed.end()) {
      pressedIt->second = true;
    }
    if (renderer != nullptr) {
      renderer->onLanePressed(selectedCandidate.lane, result.judge,
                              context.laneBeamTimeMicros);
    }
    inputTransactions.push_back(result);
    return resultBatch(result);
  }

  result.keySoundNote =
      selectFallbackKeysound(mainLane, compensateLane, inputTime);
  if (mainLaneIt != lanePressed.end()) {
    mainLaneIt->second = true;
  }
  if (renderer != nullptr) {
    renderer->onLanePressed(mainLane, JudgeResult(None, 0),
                            context.laneBeamTimeMicros);
  }
  setReplayEvent(result, ReplayEventAction::Press, mainLane, nullptr,
                 inputTime, inputTime, JudgeResult(None, 0));
  inputTransactions.push_back(result);
  return resultBatch(result);
}

RhythmLaneInputController::ResultBatch
RhythmLaneInputController::releaseLane(int lane,
                                       const InputContext &context,
                                       bool isBackSpin) {
  inputTransactions.clear();
  Result result;
  if (chart == nullptr ||
      (allowedNoteRange.has_value() &&
       context.songTimeMicros >= allowedNoteRange->endMicros)) {
    return resultBatch(result);
  }
  auto laneIt = lanePressed.find(lane);
  if (laneIt == lanePressed.end() || !laneIt->second) {
    return resultBatch(result);
  }
  laneIt->second = false;
  if (renderer != nullptr) {
    renderer->onLaneReleased(lane, context.laneBeamTimeMicros);
  }

  const long long inputTime = inputTimeMicros(context);
  const bool lr2Release =
      judge.rules().ruleset == GameplayRuleset::LR2;
  for (const auto *measure : chart->Measures) {
    if (measure == nullptr) {
      continue;
    }
    for (const auto *timeline : measure->TimeLines) {
      if (timeline == nullptr) {
        continue;
      }
      if (lane < 0 || static_cast<size_t>(lane) >= timeline->Notes.size()) {
        continue;
      }
      auto *note = timeline->Notes[lane];
      const auto *longNote =
          dynamic_cast<const bms_parser::LongNote *>(note);
      if (lr2Release &&
          (longNote == nullptr || !longNote->IsTail() ||
           !longNote->IsHolding)) {
        continue;
      }
      if (!lr2Release &&
          timeline->Timing < inputTime - latePoorTiming) {
        continue;
      }
      if (note == nullptr || note->IsPlayed || !noteAllowed(note)) {
        continue;
      }
      result = releaseNote(note, inputTime, inputTime, isBackSpin);
      result.note = note;
      if (!result.hasReplayEvent) {
        setReplayEvent(result, ReplayEventAction::Release, lane, nullptr,
                       inputTime, inputTime, result.judge);
      }
      inputTransactions.push_back(result);
      return resultBatch(result);
    }
  }

  setReplayEvent(result, ReplayEventAction::Release, lane, nullptr, inputTime,
                 inputTime, JudgeResult(None, 0));
  inputTransactions.push_back(result);
  return resultBatch(result);
}

void RhythmLaneInputController::resetLaneStates() {
  lanePressed.clear();
  acceptedLongHeadJudges.clear();
  if (chart == nullptr) {
    return;
  }
  for (const auto lane : chart->Meta.GetTotalLaneIndices()) {
    lanePressed[lane] = false;
  }
}

long long RhythmLaneInputController::inputTimeMicros(
    const InputContext &context) const {
  return context.songTimeMicros -
         static_cast<long long>(context.inputDelay * 1000000.0);
}

bool RhythmLaneInputController::noteAllowed(
    const bms_parser::Note *note) const {
  return note != nullptr &&
         (!allowedNoteRange.has_value() ||
          allowedNoteRange->contains(note));
}

RhythmLaneInputController::ResultBatch
RhythmLaneInputController::resultBatch(const Result &selected) const {
  ResultBatch result;
  result.note = selected.note;
  result.keySoundNote = selected.keySoundNote;
  result.hasJudge = selected.hasJudge;
  result.judge = selected.judge;
  result.hasReplayEvent = selected.hasReplayEvent;
  result.replayEvent = selected.replayEvent;
  result.transactions = inputTransactions;
  return result;
}

void RhythmLaneInputController::rememberAcceptedLongHeadJudge(
    bms_parser::LongNote *head, const JudgeResult &judgeResult) {
  const auto found = std::ranges::find(acceptedLongHeadJudges, head,
                                       &AcceptedLongHeadJudge::head);
  if (found != acceptedLongHeadJudges.end()) {
    found->judge = judgeResult;
    return;
  }
  acceptedLongHeadJudges.push_back({head, judgeResult});
}

JudgeResult RhythmLaneInputController::acceptedLongHeadJudge(
    const bms_parser::LongNote *tail) const {
  if (tail == nullptr || tail->Head == nullptr) {
    return JudgeResult(None, 0);
  }
  const auto found = std::ranges::find(acceptedLongHeadJudges, tail->Head,
                                       &AcceptedLongHeadJudge::head);
  if (found != acceptedLongHeadJudges.end()) {
    return found->judge;
  }
  return judge.judgeAt(
      gameplay::judgeRoleFor(tail->Head, chart->Meta, longNoteModeOverride),
      noteTimingMicros(tail->Head), tail->Head->PlayedTime);
}

RhythmLaneInputController::Result
RhythmLaneInputController::pressNote(bms_parser::Note *note,
                                     long long pressedTime,
                                     long long songTimeMicros) {
  Result result;
  result.note = note;
  if (note == nullptr) {
    return result;
  }
  result.keySoundNote = note;

  const JudgeResult judgeResult = judge.judgeAt(
      gameplay::judgeRoleFor(note, chart->Meta, longNoteModeOverride),
      noteTimingMicros(note), pressedTime);
  result.judge = judgeResult;
  if (judgeResult.judgement == None) {
    return result;
  }
  if (judgeResult.isNotePlayed()) {
    if (note->IsLongNote()) {
      auto *longNote = static_cast<bms_parser::LongNote *>(note);
      if (!longNote->IsTail()) {
        longNote->Press(pressedTime);
        rememberAcceptedLongHeadJudge(longNote, judgeResult);
        result.hasJudge =
            effectiveLongNoteIsCharge(longNote, chart, longNoteModeOverride);
        setReplayEvent(result, ReplayEventAction::Press, note->Lane, note,
                       songTimeMicros, pressedTime, judgeResult);
      }
      return result;
    }
    note->Press(pressedTime);
  }
  result.hasJudge = true;
  setReplayEvent(result, ReplayEventAction::Press, note->Lane, note,
                 songTimeMicros, pressedTime, judgeResult);
  return result;
}

RhythmLaneInputController::Result
RhythmLaneInputController::releaseNote(bms_parser::Note *note,
                                       long long releasedTime,
                                       long long songTimeMicros,
                                       bool isBackSpin) {
  Result result;
  result.note = note;
  if (note == nullptr || !note->IsLongNote()) {
    return result;
  }
  auto *longNote = static_cast<bms_parser::LongNote *>(note);
  if (!longNote->IsTail() || !longNote->IsHolding) {
    return result;
  }

  longNote->Release(releasedTime);
  const auto judgeResult = judge.judgeAt(
      gameplay::judgeRoleFor(longNote, chart->Meta, longNoteModeOverride),
      noteTimingMicros(longNote), releasedTime);
  JudgeResult appliedJudge(None, 0);
  const bool chargeLongNote =
      effectiveLongNoteIsCharge(longNote, chart, longNoteModeOverride);
  const bool scratchLongNote =
      chart != nullptr && chartLaneIsScratch(chart->Meta, note->Lane);
  if (chargeLongNote && scratchLongNote && !isBackSpin) {
    longNote->Release(releasedTime);
    const JudgeResult nonBackSpinJudge(
        Poor, releasedTime - noteTimingMicros(longNote));
    result.judge = nonBackSpinJudge;
    result.hasJudge = true;
    setReplayEvent(result, ReplayEventAction::Release, note->Lane, note,
                   songTimeMicros, releasedTime, nonBackSpinJudge);
    return result;
  }
  appliedJudge =
      chargeLongNote
          ? normalizeLongNoteReleaseJudge(judgeResult)
          : judgeClassicLongNoteRelease(judge, chart->Meta,
                                        longNoteModeOverride, longNote,
                                        releasedTime,
                                        acceptedLongHeadJudge(longNote));
  result.judge = appliedJudge;
  result.hasJudge = true;
  setReplayEvent(result, ReplayEventAction::Release, note->Lane, note,
                 songTimeMicros, releasedTime, appliedJudge);
  return result;
}
