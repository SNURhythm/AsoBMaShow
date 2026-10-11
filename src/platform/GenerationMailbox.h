#pragma once

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace platform {
// Value messages only; delivery runs on the owner after take() unlocks.
// open/close/take belong to that owner; post may run on the native UI thread.
template <typename T> class GenerationMailbox {
public:
  std::uint64_t open() {
    const std::lock_guard lock(mutex_);
    messages_.clear();
    active_ = ++generation_;
    return active_;
  }
  void close(std::uint64_t generation) {
    const std::lock_guard lock(mutex_);
    if (generation != active_) return;
    active_ = 0;
    messages_.clear();
  }
  void post(std::uint64_t generation, T message) {
    const std::lock_guard lock(mutex_);
    if (generation != 0 && generation == active_) messages_.push_back(std::move(message));
  }
  std::vector<T> take(std::uint64_t generation) {
    const std::lock_guard lock(mutex_);
    if (generation == 0 || generation != active_) return {};
    return std::exchange(messages_, {});
  }

private:
  std::mutex mutex_;
  std::uint64_t generation_ = 0;
  std::uint64_t active_ = 0;
  std::vector<T> messages_;
};
}
