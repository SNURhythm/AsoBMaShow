#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

// Coordinates objects that retain references into the active InputProfile.
// Registration::reset() shares the notification mutex, so once it returns its
// callback is neither running nor eligible for a later replacement.
class InputProfileReplacementNotifier {
private:
  struct State {
    std::mutex mutex;
    std::map<std::uint64_t, std::function<void()>> callbacks;
    std::uint64_t nextId = 1;
  };

public:
  class Registration {
  public:
    Registration() = default;
    ~Registration() { reset(); }

    Registration(const Registration &) = delete;
    Registration &operator=(const Registration &) = delete;

    Registration(Registration &&other) noexcept
        : state_(std::move(other.state_)), id_(std::exchange(other.id_, 0)) {}

    Registration &operator=(Registration &&other) noexcept {
      if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        id_ = std::exchange(other.id_, 0);
      }
      return *this;
    }

    void reset() {
      if (id_ == 0) {
        return;
      }
      if (const auto state = state_.lock()) {
        const std::lock_guard lock(state->mutex);
        state->callbacks.erase(id_);
      }
      state_.reset();
      id_ = 0;
    }

    [[nodiscard]] explicit operator bool() const { return id_ != 0; }

  private:
    friend class InputProfileReplacementNotifier;

    Registration(const std::shared_ptr<State> &state, std::uint64_t id)
        : state_(state), id_(id) {}

    std::weak_ptr<State> state_;
    std::uint64_t id_ = 0;
  };

  using Callback = std::function<void()>;

  InputProfileReplacementNotifier() = default;
  InputProfileReplacementNotifier(const InputProfileReplacementNotifier &) =
      delete;
  InputProfileReplacementNotifier &
  operator=(const InputProfileReplacementNotifier &) = delete;
  InputProfileReplacementNotifier(InputProfileReplacementNotifier &&) =
      delete;
  InputProfileReplacementNotifier &
  operator=(InputProfileReplacementNotifier &&) = delete;

  [[nodiscard]] Registration subscribe(Callback callback) {
    if (!callback) {
      return {};
    }
    const std::lock_guard lock(state_->mutex);
    const std::uint64_t id = state_->nextId++;
    state_->callbacks.emplace(id, std::move(callback));
    return Registration(state_, id);
  }

  void notifyBeforeReplacement() {
    // Callbacks are deliberately invoked under this mutex. It makes scoped
    // unregistration a lifetime barrier for scene-owned capture controllers.
    const std::lock_guard lock(state_->mutex);
    for (const auto &[id, callback] : state_->callbacks) {
      (void)id;
      callback();
    }
  }

private:
  std::shared_ptr<State> state_ = std::make_shared<State>();
};
