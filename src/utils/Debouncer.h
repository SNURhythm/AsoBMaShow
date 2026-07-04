#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

class Debouncer;

class DebounceToken {
public:
  DebounceToken() = default;

  [[nodiscard]] bool cancelled() const {
    auto state = state_.lock();
    return state == nullptr ||
           state->generation.load(std::memory_order_acquire) != generation_;
  }

  [[nodiscard]] std::uint64_t generation() const { return generation_; }

private:
  struct State {
    std::atomic<std::uint64_t> generation{0};
  };

  friend class Debouncer;

  DebounceToken(std::weak_ptr<State> state, std::uint64_t generation)
      : state_(std::move(state)), generation_(generation) {}

  std::weak_ptr<State> state_;
  std::uint64_t generation_ = 0;
};

class Debouncer {
public:
  using Clock = std::chrono::steady_clock;
  using Callback = std::function<void(const DebounceToken &)>;

  Debouncer() : state_(std::make_shared<DebounceToken::State>()) {}

  DebounceToken schedule(std::chrono::milliseconds delay, Callback callback) {
    const std::uint64_t generation =
        state_->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    pending_ = Pending{
        .due = Clock::now() + delay,
        .token = DebounceToken{state_, generation},
        .callback = std::move(callback),
    };
    return pending_->token;
  }

  void cancel() {
    state_->generation.fetch_add(1, std::memory_order_acq_rel);
    pending_.reset();
  }

  bool update(Clock::time_point now = Clock::now()) {
    if (!pending_.has_value() || pending_->due > now) {
      return false;
    }

    Pending pending = std::move(pending_.value());
    pending_.reset();
    if (!pending.token.cancelled() && pending.callback) {
      pending.callback(pending.token);
    }
    return true;
  }

  [[nodiscard]] bool pending() const { return pending_.has_value(); }

private:
  struct Pending {
    Clock::time_point due;
    DebounceToken token;
    Callback callback;
  };

  std::shared_ptr<DebounceToken::State> state_;
  std::optional<Pending> pending_;
};
