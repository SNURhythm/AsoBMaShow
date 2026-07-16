#pragma once

#include "CompiledGameplayJudge.h"
#include "GameplayDefinition.h"
#include "../../AppSettings.h"

#include <cstddef>
#include <cstdint>
#include <optional>
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

struct GameplaySimulationConfig {
  CompiledGameplayJudge judge;
  AppSettings::NotePriorityMode notePriorityMode =
      AppSettings::NotePriorityMode::Lowest;
  std::optional<GameplayTimeRange> allowedNoteRange;
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

  [[nodiscard]] const NoteRuntimeState &noteState(NoteId id) const;
  [[nodiscard]] bool lanePressed(int lane) const noexcept;
  [[nodiscard]] GameplaySearchStats lastSearchStats() const noexcept;

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

  const GameplayDefinition &definition_;
  GameplaySimulationConfig config_;
  std::vector<NoteRuntimeState> noteStates_;
  std::vector<LaneRuntimeState> laneStates_;
  GameplaySearchStats lastSearchStats_;
};

} // namespace gameplay
