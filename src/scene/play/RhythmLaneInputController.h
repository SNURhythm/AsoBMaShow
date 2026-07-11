#pragma once

#include "../../AppSettings.h"
#include "../../ReplayData.h"
#include "../../bms_parser.hpp"
#include "Judge.h"

#include <optional>
#include <unordered_map>

class BMSRenderer;

class RhythmLaneInputController {
public:
  struct InputContext {
    long long songTimeMicros = 0;
    long long laneBeamTimeMicros = 0;
    double inputDelay = 0.0;
    AppSettings::NotePriorityMode notePriorityMode =
        AppSettings::NotePriorityMode::Lowest;
  };

  struct ReplayEventResult {
    ReplayEventAction action = ReplayEventAction::Press;
    int lane = -1;
    const bms_parser::Note *note = nullptr;
    long long songTimeMicros = 0;
    long long judgeTimeMicros = 0;
    JudgeResult judge = JudgeResult(None, 0);
  };

  struct Result {
    bms_parser::Note *note = nullptr;
    bms_parser::Note *keySoundNote = nullptr;
    bool hasJudge = false;
    JudgeResult judge = JudgeResult(None, 0);
    bool hasReplayEvent = false;
    ReplayEventResult replayEvent;
  };

  RhythmLaneInputController(
      bms_parser::Chart *chart, BMSRenderer *renderer,
      std::unordered_map<int, bool> &lanePressed,
      CourseJudgementConstraint judgementConstraint =
          CourseJudgementConstraint::None,
      int longNoteModeOverride = 0,
      std::optional<NoteTimeRange> allowedNoteRange = std::nullopt);

  Result pressLane(int lane, const InputContext &context);
  Result pressLane(int mainLane, int compensateLane,
                   const InputContext &context);
  Result releaseLane(int lane, const InputContext &context,
                     bool isBackSpin = false);
  void resetLaneStates();

private:
  bms_parser::Chart *chart = nullptr;
  BMSRenderer *renderer = nullptr;
  std::unordered_map<int, bool> &lanePressed;
  int longNoteModeOverride = 0;
  Judge judge;
  std::optional<NoteTimeRange> allowedNoteRange;
  long long latePoorTiming = 0;

  long long inputTimeMicros(const InputContext &context) const;
  Result pressNote(bms_parser::Note *note, long long pressedTime,
                   long long songTimeMicros);
  Result releaseNote(bms_parser::Note *note, long long releasedTime,
                     long long songTimeMicros, bool isBackSpin);
};
