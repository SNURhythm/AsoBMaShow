#pragma once

#include "LogicalGameplayInputAdapter.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace input {

enum class RealtimePhysicalInputTransitionType { Press, Release, Command };

struct RealtimePhysicalInputTransition {
  RealtimePhysicalInputTransitionType type =
      RealtimePhysicalInputTransitionType::Press;
  int lane = 0;
  bool backSpin = false;
  bool hasReplayControl = false;
  replay::LogicalControl replayControl;
  bool replayOnly = false;
  LogicalInputTransition command;
  std::int64_t steadyTimestampMicros = 0;
};

class RealtimePhysicalInputRouter final : private IRhythmControl {
public:
  using Sink = std::function<bool(const RealtimePhysicalInputTransition &)>;

  RealtimePhysicalInputRouter(const InputProfile &,
                              std::vector<InputScope> activeScopes, Sink);
  RealtimePhysicalInputRouter(const RealtimePhysicalInputRouter &) = delete;
  RealtimePhysicalInputRouter &
  operator=(const RealtimePhysicalInputRouter &) = delete;

  void consume(const PhysicalInputEvent &, std::int64_t steadyTimestampMicros);
  void disconnectDevice(std::string_view deviceId,
                        std::int64_t steadyTimestampMicros);
  void setGameplayEnabled(bool enabled,
                          std::int64_t steadyTimestampMicros);

private:
  struct TrackedLaneState {
    bool desiredPressed = false;
    bool publishedPressed = false;
    std::optional<replay::LogicalControl> desiredReplayControl;
    std::optional<replay::LogicalControl> publishedReplayControl;
  };

  bms_parser::Note *pressLane(int mainLane, int compensateLane,
                              double inputDelay) override;
  bms_parser::Note *pressLane(int lane, double inputDelay) override;
  bms_parser::Note *releaseLane(int lane, double inputDelay,
                                bool isBackSpin) override;
  void prepare(RealtimePhysicalInputTransitionType, int lane,
               bool backSpin = false);
  void emitApplied(const LogicalGameplayInputAdapter::AppliedTransition &);
  bool emit(RealtimePhysicalInputTransitionType, int lane, bool backSpin,
            std::optional<replay::LogicalControl>);
  bool emitReplayOnly(RealtimePhysicalInputTransitionType,
                      replay::LogicalControl);
  void emitCommand(const LogicalInputTransition &);
  static std::map<int, TrackedLaneState>
  makeTrackedLanes(const InputProfile &,
                   const std::vector<InputScope> &activeScopes);

  std::mutex mutex_;
  Sink sink_;
  std::int64_t currentTimestampMicros_ = 0;
  bool gameplayEnabled_ = false;
  std::map<int, TrackedLaneState> trackedLanes_;
  std::deque<RealtimePhysicalInputTransition> pendingTransitions_;
  LogicalGameplayInputPipeline pipeline_;
};

} // namespace input
