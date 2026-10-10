#pragma once

#include <atomic>
#include <chrono>

namespace gameplay::detail {

template <typename Semaphore, typename Rep, typename Period>
void waitForGameplayWake(Semaphore &wake, std::atomic_bool &pending,
                         std::chrono::duration<Rep, Period> timeout) {
  // A timed-out wait may race a producer that has already released a token.
  // Only consuming that token permits the next producer to release another.
  if (wake.try_acquire_for(timeout)) {
    pending.store(false, std::memory_order_release);
  }
}

} // namespace gameplay::detail
