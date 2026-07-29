#pragma once

#include "RealtimeGameplayWorker.h"

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>

namespace gameplay {

struct RealtimeGameplayInputBridgeSink {
  void *context = nullptr;
  bool (*emit)(void *, const RealtimeGameplayInput &) = nullptr;
};

class RealtimeGameplayInputBridge {
public:
  RealtimeGameplayInputBridge(std::uint64_t epoch, int keyMode,
                              RealtimeGameplayInputBridgeSink sink) noexcept;

  bool prepare(RealtimeGameplayInputType type, int lane, int compensateLane,
               bool backSpin, std::int64_t steadyTimestampMicros,
               std::int64_t inputDelayMicros);
  bool emitApplied(replay::LogicalControl control, bool pressed,
                   bool replayOnly, std::int64_t steadyTimestampMicros);

private:
  struct PendingScratchHandoff {
    replay::LogicalControl released;
    std::int64_t steadyTimestampMicros = 0;
    bool active = false;
  };

  std::uint64_t epoch_ = 0;
  int keyMode_ = 7;
  RealtimeGameplayInputBridgeSink sink_;
  std::mutex mutex_;
  std::deque<RealtimeGameplayInput> pendingInputs_;
  std::array<PendingScratchHandoff, 3> pendingScratchHandoffs_{};
};

} // namespace gameplay
