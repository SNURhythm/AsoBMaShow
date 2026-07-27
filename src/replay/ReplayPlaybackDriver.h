#pragma once

#include "BeatorajaReplayCodec.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <string>

namespace replay {

inline constexpr std::size_t kDefaultReplayPlaybackEventBudget =
    kReplayLimits.maxInputTransitions + kReplayLimits.maxTouchSamples +
    kReplayLimits.maxLaneCoverEvents;

struct ReplayPlaybackSink {
  std::function<bool(const InputTransition &, std::string &)> input;
  std::function<bool(std::span<const InputTransition>, std::string &)>
      inputBatch;
  std::function<bool(const ReplayTouchSample &, std::string &)> touch;
  std::function<bool(const ReplayLaneCoverEvent &, std::string &)> laneCover;
};

struct ReplayLogicalGameplayCallbacks {
  std::function<void(int physicalLane, double inputDelaySeconds)> pressLane;
  std::function<void(int physicalLane, double inputDelaySeconds,
                     bool backSpin)>
      releaseLane;
  std::function<void(const LogicalControl &, bool pressed)> command;
};

class ReplayLogicalGameplayAdapter {
public:
  ReplayLogicalGameplayAdapter(int keyMode,
                               ReplayLogicalGameplayCallbacks callbacks);

  [[nodiscard]] bool applyBatch(
      std::span<const InputTransition> transitions,
      std::int64_t dispatchSongTimeMicros, std::string &diagnostic);
  void reset();

private:
  [[nodiscard]] bool reversesScratchAt(
      std::span<const InputTransition> transitions,
      std::size_t index) const noexcept;
  [[nodiscard]] bool apply(const InputTransition &transition,
                           bool reversesScratch,
                           std::int64_t dispatchSongTimeMicros,
                           std::string &diagnostic);

  int keyMode_ = 0;
  ReplayLogicalGameplayCallbacks callbacks_;
  std::set<int> heldLanes_;
  std::map<int, LogicalControlKind> activeScratchDirections_;
};

enum class ReplayPlaybackDriverState {
  Advanced,
  Complete,
  InvalidPlayback,
  NonMonotonicAdvance,
  TimeOutOfBounds,
  WorkLimitExceeded,
  SinkRejected,
};

struct ReplayPlaybackAdvanceOutcome {
  ReplayPlaybackDriverState state = ReplayPlaybackDriverState::InvalidPlayback;
  std::size_t deliveredEvents = 0;
  std::string diagnostic;

  [[nodiscard]] bool advanced() const noexcept {
    return state == ReplayPlaybackDriverState::Advanced ||
           state == ReplayPlaybackDriverState::Complete;
  }
};

class ReplayPlaybackDriver {
public:
  ReplayPlaybackDriver(const ReplayChartDocument &document,
                       ReplaySetupSource source,
                       ReplayLimits limits = kReplayLimits);

  [[nodiscard]] ReplayPlaybackAdvanceOutcome advanceTo(
      std::int64_t songTimeMicros, const ReplayPlaybackSink &sink,
      std::size_t eventBudget = kDefaultReplayPlaybackEventBudget);

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] bool complete() const noexcept;
  [[nodiscard]] std::int64_t currentTimeMicros() const noexcept {
    return currentTimeMicros_;
  }
  [[nodiscard]] const std::string &diagnostic() const noexcept {
    return diagnostic_;
  }

private:
  const ReplayChartDocument &document_;
  ReplayLimits limits_;
  std::size_t nextInput_ = 0;
  std::size_t nextTouch_ = 0;
  std::size_t nextLaneCover_ = 0;
  std::int64_t currentTimeMicros_ = 0;
  bool hasAdvanced_ = false;
  bool valid_ = false;
  std::string diagnostic_;
};

} // namespace replay
