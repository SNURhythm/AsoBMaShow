#pragma once

#include <chrono>
#include <cstdint>

class FramePacer {
public:
  void setCap(std::uint32_t fps);
  void reset(std::chrono::steady_clock::time_point now);
  std::chrono::steady_clock::duration
  remaining(std::chrono::steady_clock::time_point now) const;
  void framePresented(std::chrono::steady_clock::time_point now);

private:
  std::uint32_t cap = 0;
  std::chrono::steady_clock::duration framePeriod{};
  std::chrono::steady_clock::time_point nextDeadline{};
  bool deadlineInitialized = false;
};
