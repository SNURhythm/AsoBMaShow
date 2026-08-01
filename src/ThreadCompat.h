#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

#if __has_include(<stop_token>)
#include <stop_token>
#endif

#if defined(__ANDROID__) && !defined(__cpp_lib_jthread)
namespace std {

class stop_source;

class stop_token {
public:
  stop_token() noexcept = default;

  bool stop_possible() const noexcept {
    return static_cast<bool>(stopRequested_);
  }

  bool stop_requested() const noexcept {
    return stopRequested_ != nullptr &&
           stopRequested_->load(std::memory_order_acquire);
  }

private:
  friend class jthread;
  friend class stop_source;

  explicit stop_token(std::shared_ptr<std::atomic_bool> stopRequested) noexcept
      : stopRequested_(std::move(stopRequested)) {}

  std::shared_ptr<std::atomic_bool> stopRequested_;
};

class stop_source {
public:
  stop_source()
      : stopRequested_(std::make_shared<std::atomic_bool>(false)) {}

  bool stop_possible() const noexcept {
    return static_cast<bool>(stopRequested_);
  }

  bool stop_requested() const noexcept {
    return stopRequested_ != nullptr &&
           stopRequested_->load(std::memory_order_acquire);
  }

  bool request_stop() noexcept {
    return stopRequested_ != nullptr &&
           !stopRequested_->exchange(true, std::memory_order_acq_rel);
  }

  stop_token get_token() const noexcept {
    return stop_token(stopRequested_);
  }

  void swap(stop_source &other) noexcept {
    stopRequested_.swap(other.stopRequested_);
  }

private:
  std::shared_ptr<std::atomic_bool> stopRequested_;
};

inline void swap(stop_source &lhs, stop_source &rhs) noexcept {
  lhs.swap(rhs);
}

class jthread {
public:
  using id = std::thread::id;
  using native_handle_type = std::thread::native_handle_type;

  jthread() noexcept = default;

  template <class Function, class... Args>
  explicit jthread(Function &&function, Args &&...args) {
    start(std::forward<Function>(function), std::forward<Args>(args)...);
  }

  ~jthread() {
    request_stop();
    if (joinable()) {
      join();
    }
  }

  jthread(const jthread &) = delete;
  jthread &operator=(const jthread &) = delete;

  jthread(jthread &&) noexcept = default;

  jthread &operator=(jthread &&other) noexcept {
    if (this == &other) {
      return *this;
    }
    request_stop();
    if (joinable()) {
      join();
    }
    thread_ = std::move(other.thread_);
    stopRequested_ = std::move(other.stopRequested_);
    return *this;
  }

  bool joinable() const noexcept { return thread_.joinable(); }
  id get_id() const noexcept { return thread_.get_id(); }
  native_handle_type native_handle() { return thread_.native_handle(); }

  void join() { thread_.join(); }
  void detach() { thread_.detach(); }

  void swap(jthread &other) noexcept {
    thread_.swap(other.thread_);
    stopRequested_.swap(other.stopRequested_);
  }

  bool request_stop() noexcept {
    if (stopRequested_ == nullptr) {
      return false;
    }
    stopRequested_->store(true, std::memory_order_release);
    return true;
  }

  stop_token get_stop_token() const noexcept {
    return stop_token(stopRequested_);
  }

private:
  template <class Function, class... Args>
  void start(Function &&function, Args &&...args) {
    stopRequested_ = std::make_shared<std::atomic_bool>(false);
    stop_token token(stopRequested_);

    if constexpr (std::is_invocable_v<std::decay_t<Function>, stop_token,
                                      std::decay_t<Args>...>) {
      thread_ = std::thread(std::forward<Function>(function), token,
                            std::forward<Args>(args)...);
    } else {
      thread_ = std::thread(std::forward<Function>(function),
                            std::forward<Args>(args)...);
    }
  }

  std::thread thread_;
  std::shared_ptr<std::atomic_bool> stopRequested_;
};

inline void swap(jthread &lhs, jthread &rhs) noexcept { lhs.swap(rhs); }

} // namespace std
#endif
