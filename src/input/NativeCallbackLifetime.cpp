#include "NativeCallbackLifetime.h"

#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

struct NativeCallbackLifetime::State {
  explicit State(void *registeredOwner) : owner(registeredOwner) {}

  std::mutex mutex;
  std::condition_variable condition;
  void *owner = nullptr;
  std::size_t activeLeases = 0;
  bool closed = false;
};

struct NativeCallbackLifetime::Registry {
  std::mutex mutex;
  std::unordered_map<std::uintptr_t, std::shared_ptr<State>> states;
  std::uintptr_t nextToken = 1;
};

NativeCallbackLifetime::Lease::Lease(std::shared_ptr<State> state,
                                     void *owner) noexcept
    : state_(std::move(state)), owner_(owner) {}

NativeCallbackLifetime::Lease::Lease(Lease &&other) noexcept
    : state_(std::move(other.state_)),
      owner_(std::exchange(other.owner_, nullptr)) {}

NativeCallbackLifetime::Lease &
NativeCallbackLifetime::Lease::operator=(Lease &&other) noexcept {
  if (this != &other) {
    release();
    state_ = std::move(other.state_);
    owner_ = std::exchange(other.owner_, nullptr);
  }
  return *this;
}

NativeCallbackLifetime::Lease::~Lease() { release(); }

void NativeCallbackLifetime::Lease::release() noexcept {
  if (!state_) {
    owner_ = nullptr;
    return;
  }
  {
    const std::lock_guard lock(state_->mutex);
    if (state_->activeLeases > 0) {
      --state_->activeLeases;
    }
    if (state_->activeLeases == 0) {
      state_->condition.notify_all();
    }
  }
  state_.reset();
  owner_ = nullptr;
}

NativeCallbackLifetime::NativeCallbackLifetime(void *owner) {
  if (owner == nullptr) {
    throw std::invalid_argument("Native callback owner cannot be null");
  }
  state_ = std::make_shared<State>(owner);

  Registry &entries = registry();
  const std::lock_guard lock(entries.mutex);
  if (entries.nextToken == 0 ||
      entries.nextToken == std::numeric_limits<std::uintptr_t>::max()) {
    throw std::overflow_error("Native callback token space exhausted");
  }
  token_ = entries.nextToken++;
  entries.states.emplace(token_, state_);
}

NativeCallbackLifetime::~NativeCallbackLifetime() { closeAndWait(); }

void *NativeCallbackLifetime::token() const noexcept {
  return reinterpret_cast<void *>(token_);
}

void NativeCallbackLifetime::closeAndWait() noexcept {
  std::shared_ptr<State> state = state_;
  if (!state) {
    return;
  }

  {
    Registry &entries = registry();
    const std::lock_guard lock(entries.mutex);
    const auto iterator = entries.states.find(token_);
    if (iterator != entries.states.end() && iterator->second == state) {
      entries.states.erase(iterator);
    }
  }

  {
    std::unique_lock lock(state->mutex);
    state->closed = true;
    state->owner = nullptr;
    state->condition.wait(lock, [&] { return state->activeLeases == 0; });
  }
  state_.reset();
}

NativeCallbackLifetime::Lease
NativeCallbackLifetime::acquire(void *token) noexcept {
  const auto key = reinterpret_cast<std::uintptr_t>(token);
  if (key == 0) {
    return {};
  }

  std::shared_ptr<State> state;
  {
    Registry &entries = registry();
    const std::lock_guard lock(entries.mutex);
    const auto iterator = entries.states.find(key);
    if (iterator == entries.states.end()) {
      return {};
    }
    state = iterator->second;
  }

  const std::lock_guard lock(state->mutex);
  if (state->closed || state->owner == nullptr) {
    return {};
  }
  ++state->activeLeases;
  void *owner = state->owner;
  return Lease(std::move(state), owner);
}

NativeCallbackLifetime::Registry &NativeCallbackLifetime::registry() {
  static Registry *entries = new Registry();
  return *entries;
}
