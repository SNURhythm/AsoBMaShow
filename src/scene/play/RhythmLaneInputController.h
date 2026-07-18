#pragma once

#include "../../AppSettings.h"
#include "../../ReplayData.h"
#include "../../bms_parser.hpp"
#include "CompiledGameplayJudge.h"
#include "GameplayCandidateRules.h"
#include "Judge.h"

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

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

  struct ResultBatch : Result {
    std::span<const Result> transactions;
  };

  RhythmLaneInputController(
      bms_parser::Chart *chart, BMSRenderer *renderer,
      std::unordered_map<int, bool> &lanePressed, Judge effectiveJudge,
      int longNoteModeOverride = 0,
      std::optional<NoteTimeRange> allowedNoteRange = std::nullopt);
  RhythmLaneInputController(
      bms_parser::Chart *chart, BMSRenderer *renderer,
      std::unordered_map<int, bool> &lanePressed,
      gameplay::CompiledGameplayJudge effectiveJudge,
      int longNoteModeOverride = 0,
      std::optional<NoteTimeRange> allowedNoteRange = std::nullopt);

  ResultBatch pressLane(int lane, const InputContext &context);
  ResultBatch pressLane(int mainLane, int compensateLane,
                        const InputContext &context);
  Result pressLaneForPreparation(int mainLane, int compensateLane,
                                 const InputContext &context);
  ResultBatch releaseLane(int lane, const InputContext &context,
                          bool isBackSpin = false);
  Result releaseLaneForPreparation(int lane, const InputContext &context);
  void resetLaneStates();

private:
  bms_parser::Chart *chart = nullptr;
  BMSRenderer *renderer = nullptr;
  std::unordered_map<int, bool> &lanePressed;
  int longNoteModeOverride = 0;
  gameplay::CompiledGameplayJudge judge;
  std::optional<NoteTimeRange> allowedNoteRange;
  long long latePoorTiming = 0;
  std::unordered_map<int, std::vector<bms_parser::Note *>>
      keysoundNotesByLane;
  std::vector<Result> inputTransactions;
  std::vector<bms_parser::Note *> judgeCandidateNotes;
  std::vector<gameplay::JudgeCandidateDescriptor> judgeCandidates;
  std::vector<std::size_t> multiBadSourceIndices;

  void indexKeysoundNotes();
  [[nodiscard]] bms_parser::Note *
  selectFallbackKeysound(int mainLane, int compensateLane,
                         long long inputTime) const;
  long long inputTimeMicros(const InputContext &context) const;
  [[nodiscard]] bool noteAllowed(const bms_parser::Note *note) const;
  [[nodiscard]] ResultBatch resultBatch(const Result &selected) const;
  Result pressNote(bms_parser::Note *note, long long pressedTime,
                   long long songTimeMicros);
  Result releaseNote(bms_parser::Note *note, long long releasedTime,
                     long long songTimeMicros, bool isBackSpin);
};
