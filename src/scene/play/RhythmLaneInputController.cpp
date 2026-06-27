#include "RhythmLaneInputController.h"
#include "BMSRenderer.h"
#include "../../CoursePlaySession.h"

#include <array>

namespace {
struct PressLaneCandidate {
  int lane = 0;
  bms_parser::Note *note = nullptr;
  JudgeResult judge = JudgeResult(None, 0);
};

bms_parser::LongNoteType
effectiveLongNoteType(const bms_parser::LongNote *longNote,
                      const bms_parser::Chart *chart,
                      int longNoteModeOverride) {
  return resolveEffectiveLongNoteType(longNote, chart, longNoteModeOverride);
}

bool isChargeLongNoteType(bms_parser::LongNoteType type) {
  return type == bms_parser::LongNoteType::ChargeNote ||
         type == bms_parser::LongNoteType::HellChargeNote;
}

JudgeResult normalizeLongNoteReleaseJudge(const JudgeResult &judgeResult) {
  if (judgeResult.judgement == None || judgeResult.judgement == Kpoor ||
      judgeResult.judgement == Poor) {
    return JudgeResult(Bad, judgeResult.Diff);
  }
  return judgeResult;
}

JudgeResult judgeClassicLongNoteRelease(Judge &judge,
                                        bms_parser::LongNote *tail,
                                        long long releasedTime) {
  if (tail == nullptr || !tail->IsTail() || tail->Head == nullptr) {
    return JudgeResult(None, 0);
  }

  const JudgeResult headJudge =
      judge.judgeNow(tail->Head, tail->Head->PlayedTime);
  const JudgeResult tailJudge = judge.judgeNow(tail, releasedTime);
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

long long latestHittableNoteTiming(const Judge &judge, long long inputTime) {
  bool hasWindow = false;
  long long earliestWindow = 0;
  for (const auto &entry : judge.timingWindows) {
    if (!hasWindow || entry.second.first < earliestWindow) {
      earliestWindow = entry.second.first;
      hasWindow = true;
    }
  }
  return hasWindow ? inputTime - earliestWindow : inputTime;
}

bool preferByTimingWindow(const PressLaneCandidate &current,
                          const PressLaneCandidate &next,
                          long long inputTime, const Judge &judge,
                          Judgement threshold) {
  if (next.note == nullptr || next.note->IsPlayed) {
    return false;
  }
  const auto windowIt = judge.timingWindows.find(threshold);
  if (windowIt == judge.timingWindows.end()) {
    return false;
  }

  const auto &window = windowIt->second;
  const long long currentTiming = noteTimingMicros(current.note);
  const long long nextTiming = noteTimingMicros(next.note);
  return currentTiming < inputTime - window.second &&
         nextTiming <= inputTime - window.first;
}

bool shouldPreferCandidate(const PressLaneCandidate &current,
                           const PressLaneCandidate &next,
                           long long inputTime, const Judge &judge,
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
    std::unordered_map<int, bool> &lanePressed,
    CourseJudgementConstraint judgementConstraint, int longNoteModeOverride)
    : chart(chart), renderer(renderer), lanePressed(lanePressed),
      longNoteModeOverride(longNoteModeOverride),
      judge(chart != nullptr ? chart->Meta.Rank : 3) {
  judge.applyCourseJudgementConstraint(judgementConstraint);
  if (const auto it = judge.timingWindows.find(Bad);
      it != judge.timingWindows.end()) {
    latePoorTiming = it->second.second;
  }
  resetLaneStates();
}

RhythmLaneInputController::Result
RhythmLaneInputController::pressLane(int lane, const InputContext &context) {
  return pressLane(lane, lane, context);
}

RhythmLaneInputController::Result RhythmLaneInputController::pressLane(
    int mainLane, int compensateLane, const InputContext &context) {
  Result result;
  if (chart == nullptr || renderer == nullptr) {
    return result;
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
    return result;
  }

  const long long inputTime = inputTimeMicros(context);
  const long long futureCutoff = latestHittableNoteTiming(judge, inputTime);
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
        if (note == nullptr || note->IsPlayed || note->IsLandmineNote()) {
          continue;
        }
        const JudgeResult noteJudge = judge.judgeNow(note, inputTime);
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

  if (hasSelectedCandidate) {
    result = pressNote(selectedCandidate.note, inputTime, inputTime);
    result.note = selectedCandidate.note;
    if (auto pressedIt = lanePressed.find(selectedCandidate.lane);
        pressedIt != lanePressed.end()) {
      pressedIt->second = true;
    }
    renderer->onLanePressed(selectedCandidate.lane, result.judge,
                            context.laneBeamTimeMicros);
    return result;
  }

  if (mainLaneIt != lanePressed.end()) {
    mainLaneIt->second = true;
  }
  renderer->onLanePressed(mainLane, JudgeResult(None, 0),
                          context.laneBeamTimeMicros);
  setReplayEvent(result, ReplayEventAction::Press, mainLane, nullptr,
                 inputTime, inputTime, JudgeResult(None, 0));
  return result;
}

RhythmLaneInputController::Result
RhythmLaneInputController::releaseLane(int lane,
                                       const InputContext &context) {
  Result result;
  if (chart == nullptr || renderer == nullptr) {
    return result;
  }
  auto laneIt = lanePressed.find(lane);
  if (laneIt == lanePressed.end() || !laneIt->second) {
    return result;
  }
  laneIt->second = false;
  renderer->onLaneReleased(lane, context.laneBeamTimeMicros);

  const long long inputTime = inputTimeMicros(context);
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
      if (lane < 0 || static_cast<size_t>(lane) >= timeline->Notes.size()) {
        continue;
      }
      auto *note = timeline->Notes[lane];
      if (note == nullptr || note->IsPlayed) {
        continue;
      }
      result = releaseNote(note, inputTime, inputTime);
      result.note = note;
      if (!result.hasReplayEvent) {
        setReplayEvent(result, ReplayEventAction::Release, lane, nullptr,
                       inputTime, inputTime, result.judge);
      }
      return result;
    }
  }

  setReplayEvent(result, ReplayEventAction::Release, lane, nullptr, inputTime,
                 inputTime, JudgeResult(None, 0));
  return result;
}

void RhythmLaneInputController::resetLaneStates() {
  lanePressed.clear();
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

  const JudgeResult judgeResult = judge.judgeNow(note, pressedTime);
  result.judge = judgeResult;
  if (judgeResult.judgement == None) {
    return result;
  }
  if (judgeResult.isNotePlayed()) {
    if (note->IsLongNote()) {
      auto *longNote = static_cast<bms_parser::LongNote *>(note);
      if (!longNote->IsTail()) {
        longNote->Press(pressedTime);
        result.hasJudge =
            isChargeLongNoteType(effectiveLongNoteType(
                longNote, chart, longNoteModeOverride));
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
                                       long long songTimeMicros) {
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
  const auto judgeResult = judge.judgeNow(longNote, releasedTime);
  JudgeResult appliedJudge(None, 0);
  const bool chargeLongNote =
      isChargeLongNoteType(
          effectiveLongNoteType(longNote, chart, longNoteModeOverride));
  appliedJudge =
      chargeLongNote ? normalizeLongNoteReleaseJudge(judgeResult)
                     : judgeClassicLongNoteRelease(judge, longNote, releasedTime);
  result.judge = appliedJudge;
  result.hasJudge = true;
  setReplayEvent(result, ReplayEventAction::Release, note->Lane, note,
                 songTimeMicros, releasedTime, appliedJudge);
  return result;
}
