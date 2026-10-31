#pragma once
#include "QuickRetryProjection.h"

#include "../../AppSettings.h"
#include "../../ReplayData.h"
#include "../../bms_parser.hpp"
#include "CompiledGameplayJudge.h"
#include "GameplayCandidateRules.h"
#include "Judge.h"
#include "PlayfieldPresentationEvents.h"

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

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
    bms_parser::Note *gainNote = nullptr;
    float keysoundGain = 1.0F;
    bool hasJudge = false;
    JudgeResult judge = JudgeResult(None, 0);
    bool hasReplayEvent = false;
    ReplayEventResult replayEvent;
  };

  struct ResultBatch : Result {
    std::span<const Result> transactions;
  };

  RhythmLaneInputController(
      bms_parser::Chart *chart, IPlayfieldPresentationEvents *events,
      std::unordered_map<int, bool> &lanePressed, Judge effectiveJudge,
      int longNoteModeOverride = 0,
      std::optional<NoteTimeRange> allowedNoteRange = std::nullopt);
  RhythmLaneInputController(
      bms_parser::Chart *chart, IPlayfieldPresentationEvents *events,
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
  std::span<const Result> advancePendingReleases(long long songTimeMicros);
  std::span<const Result> advanceHellChargeVoices(long long songTimeMicros);
  Result onMissedNote(bms_parser::Note *note, long long songTimeMicros);
  void resetLaneStates();
  [[nodiscard]] const quick_retry::Projection &quickRetryProjection() const noexcept;
  // Replay and legacy scene presses bypass pressLane but share deferred LN state.
  void rememberAcceptedLongHeadJudge(bms_parser::LongNote *head,
                                     const JudgeResult &judgeResult);
  void resolveQuickRetryNote(const bms_parser::Note *note);
  [[nodiscard]] bool chargeTailJudgedSuccessfully(
      const bms_parser::LongNote *tail) const;

private:
  struct AcceptedLongHeadJudge {
    JudgeResult judge = JudgeResult(None, 0);
    bool pending = false;
  };

  bms_parser::Chart *chart = nullptr;
  IPlayfieldPresentationEvents *events = nullptr;
  std::unordered_map<int, bool> &lanePressed;
  int longNoteModeOverride = 0;
  gameplay::CompiledGameplayJudge judge;
  std::optional<NoteTimeRange> allowedNoteRange;
  long long latePoorTiming = 0;
  std::unordered_map<int, std::vector<bms_parser::Note *>>
      keysoundNotesByLane;
  std::unordered_map<int, std::vector<bms_parser::Note *>> hellChargeHeadsByLane;
  std::vector<Result> inputTransactions;
  std::vector<bms_parser::Note *> judgeCandidateNotes;
  std::vector<gameplay::JudgeCandidateDescriptor> judgeCandidates;
  std::vector<std::size_t> multiBadSourceIndices;
  std::unordered_map<const bms_parser::LongNote *, AcceptedLongHeadJudge> acceptedLongHeadJudges;
  quick_retry::Projection quickRetryProjection_;
  std::unordered_map<const bms_parser::LongNote *, Judgement>
      acceptedChargeTailJudges;
  std::unordered_map<int, long long> suppressedBackspinPressMicros;

  struct PendingRelease {
    bms_parser::LongNote *tail = nullptr;
    long long deadline = 0;
    long long releasedTime = 0;
    JudgeResult judge = JudgeResult(None, 0);
  };
  std::unordered_map<int, PendingRelease> pendingReleases;
  struct HeadVoiceState { bool muted = false; bool missed = false; };
  std::unordered_map<bms_parser::LongNote *, HeadVoiceState> headVoices;
  std::vector<Result> pendingTransactions;
  bms_parser::LongNote *passingHellChargeHead(int lane, long long time) const;
  void trackHeadVoice(bms_parser::Note *note);
  void setHeadVoiceGain(Result &result, bms_parser::LongNote *head, bool muted);
  void updateHellChargeVoice(Result &result, int lane, long long time, bool pressed);
  Result resolveRelease(bms_parser::LongNote *tail, const JudgeResult &applied,
                         long long releasedTime, long long songTime, bool mute);
  void indexKeysoundNotes();
  [[nodiscard]] bms_parser::Note *
  selectFallbackKeysound(int mainLane, int compensateLane,
                         long long inputTime) const;
  long long inputTimeMicros(const InputContext &context) const;
  [[nodiscard]] bool noteAllowed(const bms_parser::Note *note) const;
  [[nodiscard]] ResultBatch resultBatch(const Result &selected) const;
  [[nodiscard]] JudgeResult acceptedLongHeadJudge(
      const bms_parser::LongNote *tail) const;
  Result pressNote(bms_parser::Note *note, long long pressedTime,
                   long long songTimeMicros);
  Result releaseNote(bms_parser::Note *note, long long releasedTime,
                     long long songTimeMicros, bool isBackSpin);
};
