#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

// Remembers notifications received while the scheduler computes its next wait.
class JukeboxSchedulerWake {
public:
  [[nodiscard]] std::uint64_t capture() {
    std::lock_guard lock(mutex);
    return generation;
  }

  void notify() {
    {
      std::lock_guard lock(mutex);
      ++generation;
    }
    condition.notify_all();
  }

  // Capture before reading scheduling state. The predicate runs under this
  // mutex and must only read atomics, without acquiring other scheduler locks.
  template <typename Ready>
  bool waitFor(std::uint64_t observed, std::chrono::microseconds duration,
               Ready ready) {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, duration, [&] {
      return generation != observed || ready();
    });
  }

private:
  std::mutex mutex;
  std::condition_variable condition;
  std::uint64_t generation = 0;
};
