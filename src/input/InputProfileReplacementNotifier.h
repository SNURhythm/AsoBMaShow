#pragma once

#include <cstdint>
#include <condition_variable>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Coordinates objects that retain references into the active InputProfile.
// Reset from another thread waits for in-flight work; reset from inside the
// callback deactivates the slot immediately and lets that invocation unwind.
class InputProfileReplacementNotifier {
private:
  struct Slot {
    std::mutex mutex;
    std::condition_variable idle;
    std::function<void()> callback;
    std::map<std::thread::id, std::size_t> inFlightByThread;
    std::size_t inFlight = 0;
    bool active = true;
  };

  struct State {
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<Slot>> slots;
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
        : state_(std::move(other.state_)), slot_(std::move(other.slot_)),
          id_(std::exchange(other.id_, 0)) {}

    Registration &operator=(Registration &&other) noexcept {
      if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        slot_ = std::move(other.slot_);
        id_ = std::exchange(other.id_, 0);
      }
      return *this;
    }

    void reset() {
      if (id_ == 0 || !slot_) {
        return;
      }
      const std::uint64_t id = std::exchange(id_, 0);
      const std::shared_ptr<Slot> slot = std::move(slot_);
      if (const auto state = state_.lock()) {
        const std::lock_guard stateLock(state->mutex);
        const auto registered = state->slots.find(id);
        if (registered != state->slots.end() && registered->second == slot) {
          state->slots.erase(registered);
        }
      }
      state_.reset();

      std::function<void()> retiredCallback;
      std::unique_lock slotLock(slot->mutex);
      slot->active = false;
      retiredCallback = std::move(slot->callback);
      const auto ownCalls = slot->inFlightByThread.find(
          std::this_thread::get_id());
      const std::size_t ownCallCount =
          ownCalls == slot->inFlightByThread.end() ? 0 : ownCalls->second;
      slot->idle.wait(slotLock, [&]() {
        return slot->inFlight <= ownCallCount;
      });
      slotLock.unlock();
      retiredCallback = {};
    }

    [[nodiscard]] explicit operator bool() const { return id_ != 0; }

  private:
    friend class InputProfileReplacementNotifier;

    Registration(const std::shared_ptr<State> &state,
                 std::shared_ptr<Slot> slot, std::uint64_t id)
        : state_(state), slot_(std::move(slot)), id_(id) {}

    std::weak_ptr<State> state_;
    std::shared_ptr<Slot> slot_;
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
    auto slot = std::make_shared<Slot>();
    slot->callback = std::move(callback);
    const std::lock_guard stateLock(state_->mutex);
    const std::uint64_t id = state_->nextId++;
    state_->slots.emplace(id, slot);
    return Registration(state_, std::move(slot), id);
  }

  void notifyBeforeReplacement() {
    std::vector<std::shared_ptr<Slot>> slots;
    {
      const std::lock_guard stateLock(state_->mutex);
      slots.reserve(state_->slots.size());
      for (const auto &[id, slot] : state_->slots) {
        (void)id;
        slots.push_back(slot);
      }
    }

    const std::thread::id threadId = std::this_thread::get_id();
    for (const auto &slot : slots) {
      Callback callback;
      {
        const std::lock_guard slotLock(slot->mutex);
        if (!slot->active || !slot->callback) {
          continue;
        }
        callback = slot->callback;
        ++slot->inFlightByThread[threadId];
        ++slot->inFlight;
      }

      const auto finishCall = [&]() {
        const std::lock_guard slotLock(slot->mutex);
        const auto ownCalls = slot->inFlightByThread.find(threadId);
        if (ownCalls != slot->inFlightByThread.end()) {
          if (--ownCalls->second == 0) {
            slot->inFlightByThread.erase(ownCalls);
          }
        }
        --slot->inFlight;
        slot->idle.notify_all();
      };

      try {
        callback();
      } catch (...) {
        const std::exception_ptr error = std::current_exception();
        callback = {};
        finishCall();
        std::rethrow_exception(error);
      }
      callback = {};
      finishCall();
    }
  }

private:
  std::shared_ptr<State> state_ = std::make_shared<State>();
};
