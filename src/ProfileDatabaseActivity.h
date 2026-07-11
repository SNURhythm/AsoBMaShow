#pragma once

#include <atomic>
#include <mutex>
#include <optional>
#include <shared_mutex>

namespace profile_database_activity {
namespace detail {
inline std::shared_mutex gate;
inline std::atomic<unsigned int> activeReads{0};
inline std::atomic<unsigned int> activeWrites{0};
inline thread_local unsigned int operationDepth = 0;
inline thread_local unsigned int readDepth = 0;
inline thread_local unsigned int writeDepth = 0;
inline thread_local unsigned int switchDepth = 0;
} // namespace detail

enum class OperationKind { Read, Write };

class OperationGuard {
public:
  explicit OperationGuard(OperationKind kind) : kind_(kind) {
    ownsOperationLock_ =
        detail::switchDepth == 0 && detail::operationDepth == 0;
    if (ownsOperationLock_) {
      lock_.emplace(detail::gate);
    }

    if (kind_ == OperationKind::Read) {
      outermostKind_ = detail::readDepth == 0;
      if (outermostKind_) {
        detail::activeReads.fetch_add(1, std::memory_order_release);
      }
      ++detail::readDepth;
    } else {
      outermostKind_ = detail::writeDepth == 0;
      if (outermostKind_) {
        detail::activeWrites.fetch_add(1, std::memory_order_release);
      }
      ++detail::writeDepth;
    }
    ++detail::operationDepth;
  }

  ~OperationGuard() {
    if (kind_ == OperationKind::Read) {
      if (detail::readDepth > 0) {
        --detail::readDepth;
      }
      if (outermostKind_) {
        detail::activeReads.fetch_sub(1, std::memory_order_release);
      }
    } else {
      if (detail::writeDepth > 0) {
        --detail::writeDepth;
      }
      if (outermostKind_) {
        detail::activeWrites.fetch_sub(1, std::memory_order_release);
      }
    }
    if (detail::operationDepth > 0) {
      --detail::operationDepth;
    }
    if (ownsOperationLock_) {
      lock_.reset();
    }
  }

  OperationGuard(const OperationGuard &) = delete;
  OperationGuard &operator=(const OperationGuard &) = delete;
  OperationGuard(OperationGuard &&) = delete;
  OperationGuard &operator=(OperationGuard &&) = delete;

private:
  std::optional<std::shared_lock<std::shared_mutex>> lock_;
  OperationKind kind_;
  bool ownsOperationLock_ = false;
  bool outermostKind_ = false;
};

class ReadGuard {
public:
  ReadGuard() : operation_(OperationKind::Read) {}

  ReadGuard(const ReadGuard &) = delete;
  ReadGuard &operator=(const ReadGuard &) = delete;
  ReadGuard(ReadGuard &&) = delete;
  ReadGuard &operator=(ReadGuard &&) = delete;

private:
  OperationGuard operation_;
};

class WriteGuard {
public:
  WriteGuard() : operation_(OperationKind::Write) {}

  WriteGuard(const WriteGuard &) = delete;
  WriteGuard &operator=(const WriteGuard &) = delete;
  WriteGuard(WriteGuard &&) = delete;
  WriteGuard &operator=(WriteGuard &&) = delete;

private:
  OperationGuard operation_;
};

class SwitchGuard {
public:
  SwitchGuard() : lock_(detail::gate, std::try_to_lock) {
    if (lock_.owns_lock()) {
      ++detail::switchDepth;
      registered_ = true;
    }
  }

  ~SwitchGuard() {
    if (registered_ && detail::switchDepth > 0) {
      --detail::switchDepth;
    }
  }

  [[nodiscard]] bool ownsLock() const { return lock_.owns_lock(); }

  SwitchGuard(const SwitchGuard &) = delete;
  SwitchGuard &operator=(const SwitchGuard &) = delete;
  SwitchGuard(SwitchGuard &&) = delete;
  SwitchGuard &operator=(SwitchGuard &&) = delete;

private:
  std::unique_lock<std::shared_mutex> lock_;
  bool registered_ = false;
};

[[nodiscard]] inline bool readsActive() {
  return detail::activeReads.load(std::memory_order_acquire) != 0;
}

[[nodiscard]] inline bool writesActive() {
  return detail::activeWrites.load(std::memory_order_acquire) != 0;
}
} // namespace profile_database_activity
