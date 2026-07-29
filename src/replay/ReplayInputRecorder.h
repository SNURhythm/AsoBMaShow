#pragma once

#include "ReplayPlayback.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace replay {

struct ReplayClock {
  void *context = nullptr;
  std::optional<std::int64_t> (*mapSteadyToSong)(void *, std::int64_t) =
      nullptr;
};

[[nodiscard]] std::optional<std::vector<InputTransition>>
normalizeReplayInput(std::span<const InputTransition> input,
                     ReplayTimeBounds bounds, std::string &diagnostic,
                     const ReplayLimits &limits = kReplayLimits) noexcept;

class ReplayInputRecorder {
public:
  explicit ReplayInputRecorder(ReplayClock clock = {},
                               ReplayLimits limits = kReplayLimits);

  bool record(std::int64_t steadyTimestampMicros, LogicalControl control,
              bool pressed, std::string &diagnostic,
              bool replayOnly = false) noexcept;
  bool recordSongTime(std::int64_t songTimeMicros, LogicalControl control,
                      bool pressed, std::string &diagnostic,
                      bool replayOnly = false) noexcept;
  void fail(std::string diagnostic) noexcept;
  [[nodiscard]] std::optional<std::vector<InputTransition>>
  finish(ReplayTimeBounds bounds, std::string &diagnostic) noexcept;

private:
  bool rejectCapture(std::string &diagnostic, std::string message) noexcept;

  ReplayClock clock_;
  ReplayLimits limits_;
  std::vector<InputTransition> transitions_;
  std::string failureDiagnostic_;
  bool finished_ = false;
};

} // namespace replay
