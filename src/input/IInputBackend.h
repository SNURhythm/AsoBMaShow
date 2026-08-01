#pragma once

#include "InputTypes.h"
#include "GyroscopeTurntable.h"

#include <SDL2/SDL_events.h>

#include <functional>
#include <string>
#include <utility>

namespace input {

struct InputBackendSink {
  std::function<void(PhysicalInputEvent)> enqueueInput;
  std::function<void(InputDeviceSnapshot)> enqueueDevice;
};

} // namespace input

class IInputBackend {
public:
  virtual ~IInputBackend() = default;

  // Registry lifecycle contract: one start() call is paired with one stop()
  // call (immediately after failure or during registry shutdown).
  // stop() must synchronously revoke/join native producers; retained sink
  // functions are closable and become harmless no-ops during teardown.
  virtual bool start(std::string &errorMessage) = 0;
  virtual void stop() = 0;
  virtual void handleSdlEvent(const SDL_Event &) {}
  virtual void pump() = 0;
  // Platform realtime sources can remain dormant during ordinary UI input
  // and activate only while gameplay owns a device class.
  virtual void setRealtimeInputClaimed(input::DeviceClass, bool) {}
  virtual void configureGyroscopeTurntable(input::GyroscopeTurntableConfig) {}
  virtual void resetGyroscopeTurntableSession() {}

protected:
  explicit IInputBackend(input::InputBackendSink sink)
      : sink_(std::move(sink)) {}

  void publishInput(input::PhysicalInputEvent event) const {
    if (sink_.enqueueInput) {
      sink_.enqueueInput(std::move(event));
    }
  }

  void publishDevice(input::InputDeviceSnapshot device) const {
    if (sink_.enqueueDevice) {
      sink_.enqueueDevice(std::move(device));
    }
  }

private:
  input::InputBackendSink sink_;
};
