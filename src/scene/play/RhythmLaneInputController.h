#pragma once

#include "../../AppSettings.h"
#include "../../ReplayData.h"
#include "../../bms_parser.hpp"
#include "Judge.h"

#include <functional>
#include <unordered_map>

class BMSRenderer;

class RhythmLaneInputController {
public:
  struct Callbacks {
    std::function<long long()> currentSongTimeMicros;
    std::function<long long()> laneBeamTimeMicros;
    std::function<void(bms_parser::Note *)> playKeySound;
    std::function<void(const JudgeResult &, bool)> onJudge;
    std::function<void(ReplayEventAction, int, const bms_parser::Note *,
                       long long, long long, const JudgeResult &)>
        recordReplayEvent;
    std::function<void()> onLaneStateChanged;
    std::function<AppSettings::NotePriorityMode()> notePriorityMode;
    std::function<bool()> recordTimingSample;
  };

  RhythmLaneInputController(
      bms_parser::Chart *chart, BMSRenderer *renderer,
      std::unordered_map<int, bool> &lanePressed, Callbacks callbacks);

  bms_parser::Note *pressLane(int lane, double inputDelay = 0.0);
  bms_parser::Note *pressLane(int mainLane, int compensateLane,
                              double inputDelay = 0.0);
  bms_parser::Note *releaseLane(int lane, double inputDelay = 0.0);
  void resetLaneStates();

private:
  bms_parser::Chart *chart = nullptr;
  BMSRenderer *renderer = nullptr;
  std::unordered_map<int, bool> &lanePressed;
  Callbacks callbacks;
  Judge judge;
  long long latePoorTiming = 0;

  long long inputTimeMicros(double inputDelay) const;
  long long laneBeamTimeMicros() const;
  AppSettings::NotePriorityMode notePriorityMode() const;
  bool recordTimingSample() const;
  void notifyLaneStateChanged() const;
  void recordReplayEvent(ReplayEventAction action, int lane,
                         const bms_parser::Note *note,
                         long long songTimeMicros, long long judgeTimeMicros,
                         const JudgeResult &judgeResult) const;
  void onJudge(const JudgeResult &judgeResult) const;
  JudgeResult pressNote(bms_parser::Note *note, long long pressedTime,
                        long long songTimeMicros);
  JudgeResult releaseNote(bms_parser::Note *note, long long releasedTime,
                          long long songTimeMicros);
};
