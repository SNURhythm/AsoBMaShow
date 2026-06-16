#include "RhythmLaneInputController.h"
#include "BMSRenderer.h"

#include <array>
#include <cmath>
#include <utility>

namespace {
struct PressLaneCandidate {
  int lane = 0;
  bms_parser::Note *note = nullptr;
  JudgeResult judge = JudgeResult(None, 0);
};

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
} // namespace

RhythmLaneInputController::RhythmLaneInputController(
    bms_parser::Chart *chart, BMSRenderer *renderer,
    std::unordered_map<int, bool> &lanePressed, Callbacks callbacks)
    : chart(chart), renderer(renderer), lanePressed(lanePressed),
      callbacks(std::move(callbacks)),
      judge(chart != nullptr ? chart->Meta.Rank : 3) {
  if (const auto it = judge.timingWindows.find(Bad);
      it != judge.timingWindows.end()) {
    latePoorTiming = it->second.second;
  }
  resetLaneStates();
}

bms_parser::Note *RhythmLaneInputController::pressLane(int lane,
                                                       double inputDelay) {
  return pressLane(lane, lane, inputDelay);
}

bms_parser::Note *
RhythmLaneInputController::pressLane(int mainLane, int compensateLane,
                                     double inputDelay) {
  if (chart == nullptr || renderer == nullptr) {
    return nullptr;
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
    return nullptr;
  }

  const long long inputTime = inputTimeMicros(inputDelay);
  const long long futureCutoff = latestHittableNoteTiming(judge, inputTime);
  const AppSettings::NotePriorityMode priorityMode = notePriorityMode();
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
                                  judge, priorityMode)) {
          selectedCandidate = candidate;
          hasSelectedCandidate = true;
        }
        if (priorityMode == AppSettings::NotePriorityMode::Lowest) {
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

  const long long beamTime = laneBeamTimeMicros();
  if (hasSelectedCandidate) {
    const JudgeResult judgement =
        pressNote(selectedCandidate.note, inputTime, inputTime);
    if (auto pressedIt = lanePressed.find(selectedCandidate.lane);
        pressedIt != lanePressed.end()) {
      pressedIt->second = true;
    }
    notifyLaneStateChanged();
    renderer->onLanePressed(selectedCandidate.lane, judgement, beamTime);
    return selectedCandidate.note;
  }

  if (mainLaneIt != lanePressed.end()) {
    mainLaneIt->second = true;
  }
  notifyLaneStateChanged();
  renderer->onLanePressed(mainLane, JudgeResult(None, 0), beamTime);
  recordReplayEvent(ReplayEventAction::Press, mainLane, nullptr, inputTime,
                    inputTime, JudgeResult(None, 0));
  return nullptr;
}

bms_parser::Note *RhythmLaneInputController::releaseLane(int lane,
                                                         double inputDelay) {
  if (chart == nullptr || renderer == nullptr) {
    return nullptr;
  }
  auto laneIt = lanePressed.find(lane);
  if (laneIt == lanePressed.end() || !laneIt->second) {
    return nullptr;
  }
  laneIt->second = false;
  notifyLaneStateChanged();
  renderer->onLaneReleased(lane, laneBeamTimeMicros());

  const long long inputTime = inputTimeMicros(inputDelay);
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
      const JudgeResult releaseJudge = releaseNote(note, inputTime, inputTime);
      if (releaseJudge.judgement == None) {
        recordReplayEvent(ReplayEventAction::Release, lane, nullptr, inputTime,
                          inputTime, releaseJudge);
      }
      return note;
    }
  }

  recordReplayEvent(ReplayEventAction::Release, lane, nullptr, inputTime,
                    inputTime, JudgeResult(None, 0));
  return nullptr;
}

void RhythmLaneInputController::resetLaneStates() {
  lanePressed.clear();
  if (chart == nullptr) {
    return;
  }
  const long long beamTime = laneBeamTimeMicros();
  for (const auto lane : chart->Meta.GetTotalLaneIndices()) {
    lanePressed[lane] = false;
    if (renderer != nullptr) {
      renderer->onLaneReleased(lane, beamTime);
    }
  }
  notifyLaneStateChanged();
}

long long RhythmLaneInputController::inputTimeMicros(double inputDelay) const {
  const long long baseTime =
      callbacks.currentSongTimeMicros ? callbacks.currentSongTimeMicros() : 0;
  return baseTime - static_cast<long long>(inputDelay * 1000000.0);
}

long long RhythmLaneInputController::laneBeamTimeMicros() const {
  return callbacks.laneBeamTimeMicros ? callbacks.laneBeamTimeMicros()
                                      : inputTimeMicros(0.0);
}

AppSettings::NotePriorityMode
RhythmLaneInputController::notePriorityMode() const {
  return callbacks.notePriorityMode ? callbacks.notePriorityMode()
                                    : AppSettings::NotePriorityMode::Lowest;
}

bool RhythmLaneInputController::recordTimingSample() const {
  return callbacks.recordTimingSample ? callbacks.recordTimingSample() : true;
}

void RhythmLaneInputController::notifyLaneStateChanged() const {
  if (callbacks.onLaneStateChanged) {
    callbacks.onLaneStateChanged();
  }
}

void RhythmLaneInputController::recordReplayEvent(
    ReplayEventAction action, int lane, const bms_parser::Note *note,
    long long songTimeMicros, long long judgeTimeMicros,
    const JudgeResult &judgeResult) const {
  if (callbacks.recordReplayEvent) {
    callbacks.recordReplayEvent(action, lane, note, songTimeMicros,
                                judgeTimeMicros, judgeResult);
  }
}

void RhythmLaneInputController::onJudge(const JudgeResult &judgeResult) const {
  if (callbacks.onJudge) {
    callbacks.onJudge(judgeResult, recordTimingSample());
  }
}

JudgeResult RhythmLaneInputController::pressNote(bms_parser::Note *note,
                                                 long long pressedTime,
                                                 long long songTimeMicros) {
  if (note == nullptr) {
    return JudgeResult(None, 0);
  }
  if (callbacks.playKeySound) {
    callbacks.playKeySound(note);
  }

  const JudgeResult judgeResult = judge.judgeNow(note, pressedTime);
  if (judgeResult.judgement == None) {
    return judgeResult;
  }
  if (judgeResult.isNotePlayed()) {
    if (note->IsLongNote()) {
      auto *longNote = static_cast<bms_parser::LongNote *>(note);
      if (!longNote->IsTail()) {
        longNote->Press(pressedTime);
        recordReplayEvent(ReplayEventAction::Press, note->Lane, note,
                          songTimeMicros, pressedTime, judgeResult);
      }
      return judgeResult;
    }
    note->Press(pressedTime);
  }
  onJudge(judgeResult);
  recordReplayEvent(ReplayEventAction::Press, note->Lane, note, songTimeMicros,
                    pressedTime, judgeResult);
  return judgeResult;
}

JudgeResult RhythmLaneInputController::releaseNote(bms_parser::Note *note,
                                                   long long releasedTime,
                                                   long long songTimeMicros) {
  if (note == nullptr || !note->IsLongNote()) {
    return JudgeResult(None, 0);
  }
  auto *longNote = static_cast<bms_parser::LongNote *>(note);
  if (!longNote->IsTail() || !longNote->IsHolding) {
    return JudgeResult(None, 0);
  }

  longNote->Release(releasedTime);
  const auto judgeResult = judge.judgeNow(longNote, releasedTime);
  JudgeResult appliedJudge(None, 0);
  if (judgeResult.judgement == None || judgeResult.judgement == Kpoor ||
      judgeResult.judgement == Poor) {
    appliedJudge = JudgeResult(Bad, judgeResult.Diff);
  } else {
    appliedJudge = judge.judgeNow(longNote->Head, longNote->Head->PlayedTime);
  }
  onJudge(appliedJudge);
  recordReplayEvent(ReplayEventAction::Release, note->Lane, note,
                    songTimeMicros, releasedTime, appliedJudge);
  return appliedJudge;
}
