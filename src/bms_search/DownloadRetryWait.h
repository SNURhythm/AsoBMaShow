#pragma once

#include <atomic>
#include <chrono>
#include <thread>

// Keep the monotonic clock and sleeper replaceable together: retry tests can
// exercise the complete backoff and cancellation loop without wall-clock waits.
template <typename Now = decltype(&std::chrono::steady_clock::now),
          typename Sleep = void (*)(std::chrono::milliseconds)>
bool waitForDownloadRetry(
    int attempt, const std::atomic_bool &cancelled,
    Now now = &std::chrono::steady_clock::now,
    Sleep sleep = +[](std::chrono::milliseconds duration) {
      std::this_thread::sleep_for(duration);
    }) {
  const auto retryAt = now() + std::chrono::seconds(1 << attempt);
  while (now() < retryAt) {
    if (cancelled.load()) return false;
    sleep(std::chrono::milliseconds(100));
  }
  return true;
}
