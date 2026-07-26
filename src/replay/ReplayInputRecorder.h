#pragma once

#include "ReplayPlaybackData.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace replay {

struct ReplayInputRecorderLimits {
  std::int64_t minimumSongTimeMicros = kMinimumReplaySongTimeMicros;
  std::size_t maximumTransitions = 1'000'000;
};

struct ReplayClock {
  void *context = nullptr;
  std::optional<std::int64_t> (*mapSteadyToSong)(void *,
                                                 std::int64_t) = nullptr;
};

class ReplayInputRecorder {
public:
  explicit ReplayInputRecorder(ReplayClock, ReplayInputRecorderLimits = {});

  bool record(std::int64_t steadyTimestampMicros, LogicalControl control,
              bool pressed, std::string &diagnostic) noexcept;
  bool recordSongTime(std::int64_t songTimeMicros, LogicalControl control,
                      bool pressed, std::string &diagnostic) noexcept;
  std::optional<std::vector<InputTransition>>
  finish(std::string &diagnostic) noexcept;

private:
  struct ControlState {
    LogicalControl control;
    bool pressed = false;
  };

  bool rejectCapture(std::string &diagnostic, std::string message);

  ReplayClock clock_;
  ReplayInputRecorderLimits limits_;
  std::vector<InputTransition> transitions_;
  std::vector<ControlState> states_;
  std::optional<std::int64_t> lastSongTimeMicros_;
  std::string failureDiagnostic_;
  bool finished_ = false;
};

class ReplayInputCaptureBuffer {
public:
  explicit ReplayInputCaptureBuffer(ReplayInputRecorderLimits limits = {});

  bool capture(InputTransition transition, std::string &diagnostic) noexcept;
  void fail(std::string diagnostic) noexcept;
  std::optional<std::vector<InputTransition>>
  finish(std::string &diagnostic) noexcept;

private:
  struct ControlTimestamp {
    LogicalControl control;
    std::int64_t songTimeMicros = 0;
  };

  ReplayInputRecorderLimits limits_;
  std::vector<InputTransition> pending_;
  std::vector<ControlTimestamp> controlTimestamps_;
  std::string failureDiagnostic_;
  bool finished_ = false;
};

} // namespace replay
