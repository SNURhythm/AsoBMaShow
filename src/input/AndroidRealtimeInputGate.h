#pragma once

#include "RealtimePhysicalInputRouter.h"

#include <set>
#include <string>

namespace input {

// The gameplay session serializes producer callbacks and lifecycle changes
// with its lifecycle mutex. Only devices observed by this ingress are tracked;
// a render-thread device snapshot may not yet contain a newly connected source.
class AndroidRealtimeInputGate {
public:
  explicit AndroidRealtimeInputGate(RealtimePhysicalInputRouter &router)
      : router_(router) {}

  void consume(const PhysicalInputEvent &event, std::int64_t time) {
    if (!focused_ || time < inputBoundaryMicros_ ||
        (event.control.deviceClass == DeviceClass::Keyboard &&
         (keyboardTextFocused_ || time < keyboardBoundaryMicros_))) return;
    devices_.insert(event.control.deviceId);
    router_.consume(event, time);
  }

  void disconnectDevice(std::string_view id, std::int64_t time) {
    router_.disconnectDevice(id, time);
    devices_.erase(std::string(id));
  }

  void setEnabled(bool enabled, std::int64_t time) {
    if (enabled_ == enabled) return;
    // Pause keeps command bindings available. Clear their accumulated lane
    // state before resuming so a paused press cannot become a gameplay edge.
    cancel(time);
    inputBoundaryMicros_ = time;
    enabled_ = enabled;
    router_.setGameplayEnabled(enabled_ && focused_, time);
  }

  void setFocused(bool focused, std::int64_t time) {
    if (focused_ == focused) return;
    cancel(time);
    inputBoundaryMicros_ = time;
    focused_ = focused;
    router_.setGameplayEnabled(enabled_ && focused_, time);
  }

  void setKeyboardTextFocused(bool focused, std::int64_t time) {
    if (keyboardTextFocused_ == focused) return;
    if (focused) disconnectDevice("keyboard", time);
    keyboardBoundaryMicros_ = time;
    keyboardTextFocused_ = focused;
  }

private:
  void cancel(std::int64_t time) {
    for (const auto &id : devices_) router_.disconnectDevice(id, time);
    devices_.clear();
  }

  RealtimePhysicalInputRouter &router_;
  std::set<std::string> devices_;
  std::int64_t inputBoundaryMicros_ = 0;
  std::int64_t keyboardBoundaryMicros_ = 0;
  bool enabled_ = false;
  bool focused_ = true;
  bool keyboardTextFocused_ = false;
};

} // namespace input
