#pragma once

#include "InputTypes.h"
#include <SDL3/SDL_scancode.h>
#include <array>
#include <bitset>
#include <functional>
#include <mutex>
#include <utility>

// Shared by native producers and the main-thread ownership/focus lifecycle.
// Only effective edges reach the registry; no debounce or event buffering.
class NativeKeyboardInputState {
public:
  static constexpr std::size_t kMaxDevices = 32;
  explicit NativeKeyboardInputState(
      std::function<void(input::PhysicalInputEvent)> emit)
      : emit_(std::move(emit)) {}

  void setClaimed(bool claimed, std::uint64_t timestampMicros) {
    const std::lock_guard lock(mutex_);
    claimed_ = claimed;
    setEnabledLocked(claimed && focused_, timestampMicros);
  }

  void setFocused(bool focused, std::uint64_t timestampMicros) {
    const std::lock_guard lock(mutex_);
    if (timestampMicros < enabledSinceMicros_ || focused_ == focused) return;
    focused_ = focused;
    setEnabledLocked(claimed_ && focused, timestampMicros);
  }

  void consume(std::size_t device, int key, bool pressed,
               std::uint64_t timestampMicros) {
    const std::lock_guard lock(mutex_);
    if (!enabled_ || timestampMicros < enabledSinceMicros_ ||
        device >= devices_.size() ||
        key <= SDL_SCANCODE_UNKNOWN || key >= SDL_SCANCODE_COUNT) return;
    const bool previous = held(key);
    devices_[device].set(key, pressed);
    if (held(key) != previous) emit(key, !previous, timestampMicros);
  }

  void disconnect(std::size_t device, std::uint64_t timestampMicros) {
    const std::lock_guard lock(mutex_);
    if (device >= devices_.size()) return;
    const auto previous = devices_[device];
    devices_[device].reset();
    for (int key = 1; key < SDL_SCANCODE_COUNT; ++key) {
      if (previous[key] && !held(key)) emit(key, false, timestampMicros);
    }
  }

private:
  void setEnabledLocked(bool enabled, std::uint64_t timestampMicros) {
    // A delayed per-process event must not reopen an earlier focus epoch.
    if (enabled && timestampMicros < enabledSinceMicros_) return;
    if (!enabled) enabledSinceMicros_ = timestampMicros;
    if (enabled_ == enabled) return;
    if (!enabled) {
      for (int key = 1; key < SDL_SCANCODE_COUNT; ++key) {
        if (held(key)) emit(key, false, timestampMicros);
      }
      for (auto &device : devices_) device.reset();
    }
    enabled_ = enabled;
    enabledSinceMicros_ = timestampMicros;
  }

  bool held(int key) const {
    for (const auto &device : devices_) {
      if (device[key]) return true;
    }
    return false;
  }

  void emit(int key, bool pressed, std::uint64_t timestampMicros) {
    emit_({.control = {.deviceId = "keyboard",
                       .deviceClass = input::DeviceClass::Keyboard,
                       .kind = input::ControlKind::Key,
                       .index = key},
           .rawValue = pressed ? 1.0 : 0.0,
           .normalizedValue = pressed ? 1.0F : 0.0F,
           .timestampMicros = timestampMicros});
  }

  std::mutex mutex_;
  bool enabled_ = false;
  bool claimed_ = false;
  bool focused_ = true;
  std::uint64_t enabledSinceMicros_ = 0;
  std::array<std::bitset<SDL_SCANCODE_COUNT>, kMaxDevices> devices_{};
  std::function<void(input::PhysicalInputEvent)> emit_;
};
