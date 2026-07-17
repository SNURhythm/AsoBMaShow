#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

// SDL owns persistent controller identities. Platform realtime sources use
// this small synchronized bridge so their events keep the same binding IDs.
class RealtimeControllerDeviceMap {
public:
  static constexpr int kMaxPlayers = 4;

  void assign(int playerIndex, std::string stableId) {
    if (!valid(playerIndex)) {
      return;
    }
    const auto index = static_cast<std::size_t>(playerIndex);
    {
      const std::lock_guard lock(mutex_);
      stableIds_[index] =
          std::make_shared<const std::string>(std::move(stableId));
    }
    generations_[index].fetch_add(1, std::memory_order_release);
  }

  void clear(int playerIndex, std::string_view stableId) {
    if (!valid(playerIndex)) {
      return;
    }
    const auto index = static_cast<std::size_t>(playerIndex);
    bool changed = false;
    {
      const std::lock_guard lock(mutex_);
      auto &current = stableIds_[index];
      if (current && *current == stableId) {
        current.reset();
        changed = true;
      }
    }
    if (changed) {
      generations_[index].fetch_add(1, std::memory_order_release);
    }
  }

  [[nodiscard]] std::shared_ptr<const std::string>
  stableId(int playerIndex) const noexcept {
    if (!valid(playerIndex)) {
      return {};
    }
    const std::lock_guard lock(mutex_);
    return stableIds_[static_cast<std::size_t>(playerIndex)];
  }

  [[nodiscard]] std::uint64_t generation(int playerIndex) const noexcept {
    if (!valid(playerIndex)) {
      return 0;
    }
    return generations_[static_cast<std::size_t>(playerIndex)].load(
        std::memory_order_acquire);
  }

  void setKeyboardRealtimeAvailable(bool available) noexcept {
    keyboardRealtimeAvailable_.store(available, std::memory_order_release);
  }

  void setControllerRealtimeAvailable(bool available) noexcept {
    controllerRealtimeAvailable_.store(available, std::memory_order_release);
  }

  [[nodiscard]] bool keyboardRealtimeAvailable() const noexcept {
    return keyboardRealtimeAvailable_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool controllerRealtimeAvailable() const noexcept {
    return controllerRealtimeAvailable_.load(std::memory_order_acquire);
  }

private:
  static constexpr bool valid(int playerIndex) {
    return playerIndex >= 0 && playerIndex < kMaxPlayers;
  }

  mutable std::mutex mutex_;
  std::array<std::shared_ptr<const std::string>, kMaxPlayers> stableIds_{};
  std::array<std::atomic<std::uint64_t>, kMaxPlayers> generations_{};
  std::atomic_bool keyboardRealtimeAvailable_ = false;
  std::atomic_bool controllerRealtimeAvailable_ = false;
};
