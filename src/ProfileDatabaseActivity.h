#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <shared_mutex>

namespace profile_database_activity {
namespace detail {
inline std::shared_mutex gate;
inline std::atomic<unsigned int> activeWrites{0};
inline thread_local unsigned int writeDepth = 0;
} // namespace detail

class WriteGuard {
public:
  WriteGuard() {
    outermost_ = detail::writeDepth == 0;
    if (outermost_) {
      lock_.emplace(detail::gate);
      detail::activeWrites.fetch_add(1, std::memory_order_release);
    }
    ++detail::writeDepth;
  }

  ~WriteGuard() {
    if (detail::writeDepth > 0) {
      --detail::writeDepth;
    }
    if (outermost_) {
      detail::activeWrites.fetch_sub(1, std::memory_order_release);
      lock_.reset();
    }
  }

  WriteGuard(const WriteGuard &) = delete;
  WriteGuard &operator=(const WriteGuard &) = delete;
  WriteGuard(WriteGuard &&) = delete;
  WriteGuard &operator=(WriteGuard &&) = delete;

private:
  std::optional<std::shared_lock<std::shared_mutex>> lock_;
  bool outermost_ = false;
};

class SwitchGuard {
public:
  SwitchGuard() : lock_(detail::gate, std::try_to_lock) {}

  [[nodiscard]] bool ownsLock() const { return lock_.owns_lock(); }

  SwitchGuard(const SwitchGuard &) = delete;
  SwitchGuard &operator=(const SwitchGuard &) = delete;
  SwitchGuard(SwitchGuard &&) noexcept = default;
  SwitchGuard &operator=(SwitchGuard &&) noexcept = default;

private:
  std::unique_lock<std::shared_mutex> lock_;
};

[[nodiscard]] inline bool writesActive() {
  return detail::activeWrites.load(std::memory_order_acquire) != 0;
}
} // namespace profile_database_activity
