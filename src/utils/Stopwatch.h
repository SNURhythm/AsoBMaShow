//
// Created by XF on 9/2/2024.
//

#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
class Stopwatch {
public:
  Stopwatch() = default;

  void start();

  void pause();

  void resume();

  void reset();

  long long elapsedMicros() const;

  void seek(long long micro);

  bool isRunning() const;

private:
  static int64_t nowMicrosInternal();
  std::atomic<int64_t> start_time_us{0};
  std::atomic<int64_t> elapsed_time_us{0};
  std::atomic<bool> running{false};
};
