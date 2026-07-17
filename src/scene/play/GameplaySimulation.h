#pragma once

#include "CompiledGameplayJudge.h"
#include "GameplayDefinition.h"
#include "GameplayScoreState.h"
#include "../../AppSettings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace gameplay {

struct NoteRuntimeState {
  bool played = false;
  bool dead = false;
  bool holding = false;
  std::int64_t playedTimeMicros = 0;
  std::int64_t releaseTimeMicros = 0;
};

struct GameplayTimeRange {
  std::int64_t startMicros = 0;
  std::int64_t endMicros = 0;

  [[nodiscard]] bool contains(std::int64_t timingMicros) const noexcept {
    return startMicros <= timingMicros && timingMicros < endMicros;
  }
};

struct GameplayAttemptOptions {
  GaugeType initialGaugeType = GaugeType::Normal;
  GaugeAutoShiftMode gaugeAutoShift = GaugeAutoShiftMode::None;
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  GaugeType gaugeAutoShiftLowerBound = GaugeType::AssistedEasy;
  std::optional<int> startingGaugePercent;
  std::optional<GaugeStateSnapshot> carriedGauge;
  int carriedCombo = 0;
  int carriedMaxCombo = 0;
  bool assistClearMark = false;
  bool autoPlay = false;
  std::size_t replayCapacity = 4096;
  std::size_t automaticResultCapacity = 4096;
};

struct GameplayAttemptSnapshot {
  std::array<int, JudgementCount> judgeCounts{};
  int combo = 0;
  int maxCombo = 0;
  int comboBreak = 0;
  int score = 0;
  float gauge = 0.0F;
  GaugeType gaugeType = GaugeType::Normal;
  int clearTypeRank = kClearTypeFailedRank;
};

struct GameplaySimulationConfig {
  CompiledGameplayJudge judge;
  AppSettings::NotePriorityMode notePriorityMode =
      AppSettings::NotePriorityMode::Lowest;
  std::optional<GameplayTimeRange> allowedNoteRange;
  GameplayAttemptOptions attempt;
};

struct GameplayInputContext {
  std::int64_t songTimeMicros = 0;
  std::int64_t laneBeamTimeMicros = 0;
  std::int64_t inputDelayMicros = 0;
};

enum class GameplayReplayAction { Press, Release, Miss, Mine, Gauge };

struct GameplayReplayEvent {
  GameplayReplayAction action = GameplayReplayAction::Press;
  NoteId noteId = kInvalidNoteId;
  int lane = -1;
  std::int64_t noteTimeMicros = -1;
  std::int64_t songTimeMicros = 0;
  std::int64_t judgeTimeMicros = 0;
  Judgement judgement = None;
  std::int64_t diffMicros = 0;
  float gauge = 0.0F;
  GaugeType gaugeType = GaugeType::Normal;
  int combo = 0;
  int score = 0;

  bool operator==(const GameplayReplayEvent &) const = default;
};

enum class LaneVisualAction { Press, Release };

struct LaneVisualEvent {
  LaneVisualAction action = LaneVisualAction::Press;
  int lane = -1;
  std::int64_t visualTimeMicros = 0;
  JudgeResult judge = JudgeResult(None, 0);
};

struct GameplayInputResult {
  NoteId noteId = kInvalidNoteId;
  NoteId soundNoteId = kInvalidNoteId;
  bool hasJudge = false;
  JudgeResult judge = JudgeResult(None, 0);
  bool hasReplayEvent = false;
  GameplayReplayEvent replayEvent;
  bool hasLaneVisual = false;
  LaneVisualEvent laneVisual;
};

struct GameplayAdvanceResult {
  std::span<const GameplayInputResult> transactions;
  std::int64_t advancedToMicros = 0;
};

struct GameplaySearchStats {
  std::size_t notesExamined = 0;
};

class GameplaySimulation {
public:
  GameplaySimulation(const GameplayDefinition &definition,
                     GameplaySimulationConfig config);

  GameplayInputResult pressLane(int lane, const GameplayInputContext &context);
  GameplayInputResult pressLane(int mainLane, int compensateLane,
                                const GameplayInputContext &context);
  GameplayInputResult releaseLane(int lane, const GameplayInputContext &context,
                                  bool isBackSpin = false);
  GameplayAdvanceResult advanceTo(std::int64_t songTimeMicros,
                                  std::int64_t visualTimeMicros);
  GameplayInputResult applyPressAt(int mainLane, int compensateLane,
                                   const GameplayInputContext &context);
  GameplayInputResult applyReleaseAt(int lane,
                                     const GameplayInputContext &context,
                                     bool isBackSpin = false);
  GameplayAdvanceResult
  finalizePracticeRange(std::int64_t finalizationTimeMicros,
                        std::int64_t visualTimeMicros);

  [[nodiscard]] const NoteRuntimeState &noteState(NoteId id) const;
  [[nodiscard]] bool lanePressed(int lane) const noexcept;
  [[nodiscard]] GameplaySearchStats lastSearchStats() const noexcept;
  [[nodiscard]] GameplaySearchStats lastAdvanceStats() const noexcept;
  [[nodiscard]] const GameplayScoreState &scoreState() const noexcept;
  [[nodiscard]] GameplayAttemptSnapshot snapshot() const noexcept;
  [[nodiscard]] std::span<const GameplayReplayEvent>
  replayEvents() const noexcept;
  [[nodiscard]] bool replayOverflowed() const noexcept;
  [[nodiscard]] std::span<const GameplayInputResult>
  automaticResults() const noexcept;
  [[nodiscard]] bool automaticResultOverflowed() const noexcept;
  [[nodiscard]] std::span<const std::int64_t>
  hellChargeBalances() const noexcept;

private:
  struct LaneRuntimeState {
    int lane = -1;
    bool pressed = false;
    std::size_t cursor = 0;
  };

  [[nodiscard]] LaneRuntimeState *findLane(int lane) noexcept;
  [[nodiscard]] const LaneRuntimeState *findLane(int lane) const noexcept;
  [[nodiscard]] std::int64_t
  inputTime(const GameplayInputContext &context) const noexcept;
  [[nodiscard]] NoteId selectPressCandidate(int mainLane, int compensateLane,
                                            std::int64_t inputTimeMicros);
  [[nodiscard]] NoteId selectReleaseCandidate(int lane,
                                              std::int64_t inputTimeMicros);
  void commitJudge(const JudgeResult &judge);
  bool recordReplay(GameplayReplayEvent &event);
  bool recordAutomaticResult(const GameplayInputResult &result);
  void markMissed(NoteId id, std::int64_t judgeTimeMicros, bool dead);
  void clearPairHolding(NoteId id);
  GameplayInputResult commitMiss(NoteId id, std::int64_t songTimeMicros,
                                 std::int64_t judgeTimeMicros);
  GameplayInputResult commitMiss(NoteId id, std::int64_t songTimeMicros,
                                 std::int64_t judgeTimeMicros,
                                 const JudgeResult &judge);
  GameplayInputResult commitAutomaticRelease(NoteId tailId,
                                             std::int64_t songTimeMicros,
                                             std::int64_t visualTimeMicros);
  void initializeAt(std::int64_t startMicros);
  [[nodiscard]] bool noteAllowed(NoteId id) const noexcept;
  void processAtTiming(NoteId id, std::int64_t songTimeMicros,
                       std::int64_t visualTimeMicros);
  void processLatePoor(NoteId id, std::int64_t songTimeMicros);
  [[nodiscard]] bool hellChargeActiveAt(NoteId headId,
                                        std::int64_t timeMicros) const;
  void integrateHellChargeInterval(std::int64_t fromMicros,
                                   std::int64_t toMicros);
  void commitGaugeTick(Judgement judgement,
                       std::int64_t songTimeMicros);

  const GameplayDefinition &definition_;
  GameplaySimulationConfig config_;
  GameplayScoreState scoreState_;
  std::vector<NoteRuntimeState> noteStates_;
  std::vector<std::int64_t> hellChargeBalanceMicros_;
  std::vector<LaneRuntimeState> laneStates_;
  std::vector<GameplayReplayEvent> replayEvents_;
  std::vector<GameplayInputResult> automaticResults_;
  bool replayOverflowed_ = false;
  bool automaticResultOverflowed_ = false;
  std::size_t atTimingCursor_ = 0;
  std::size_t latePoorCursor_ = 0;
  std::int64_t lastAdvancedMicros_ = 0;
  bool hasAdvanced_ = false;
  bool practiceRangeFinalized_ = false;
  GameplaySearchStats lastSearchStats_;
  GameplaySearchStats lastAdvanceStats_;
};

} // namespace gameplay
