#pragma once

#include "LogicalGameplayInputAdapter.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string_view>
#include <vector>

namespace input {

enum class RealtimePhysicalInputTransitionType { Press, Release, Command };

struct RealtimePhysicalInputTransition {
  RealtimePhysicalInputTransitionType type =
      RealtimePhysicalInputTransitionType::Press;
  int lane = 0;
  bool backSpin = false;
  replay::LogicalControlKind replayControl =
      replay::LogicalControlKind::Lane;
  LogicalInputTransition command;
  std::int64_t steadyTimestampMicros = 0;
};

class RealtimePhysicalInputRouter final : private IRhythmControl {
public:
  static constexpr std::size_t kTrackedLaneCapacity = 64;
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
  bms_parser::Note *pressLane(int mainLane, int compensateLane,
                              double inputDelay) override;
  bms_parser::Note *pressLane(int lane, double inputDelay) override;
  bms_parser::Note *releaseLane(int lane, double inputDelay,
                                bool isBackSpin) override;
  bool emit(RealtimePhysicalInputTransitionType, int lane,
            bool backSpin = false,
            replay::LogicalControlKind replayControl =
                replay::LogicalControlKind::Lane);
  void emitApplied(const LogicalGameplayInputAdapter::AppliedTransition &);
  void prepare(RealtimePhysicalInputTransitionType, int lane,
               bool backSpin = false);
  void emitCommand(const LogicalInputTransition &);

  std::mutex mutex_;
  Sink sink_;
  std::int64_t currentTimestampMicros_ = 0;
  bool gameplayEnabled_ = false;
  std::array<bool, kTrackedLaneCapacity> desiredLanePressed_{};
  std::array<bool, kTrackedLaneCapacity> publishedLanePressed_{};
  std::array<replay::LogicalControlKind, kTrackedLaneCapacity>
      desiredReplayControls_{};
  std::deque<RealtimePhysicalInputTransition> pendingTransitions_;
  LogicalGameplayInputPipeline pipeline_;
};

} // namespace input
