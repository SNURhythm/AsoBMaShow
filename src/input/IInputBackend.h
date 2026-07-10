#pragma once

#include "InputTypes.h"

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

  virtual bool start(std::string &errorMessage) = 0;
  virtual void stop() = 0;
  virtual void handleSdlEvent(const SDL_Event &) {}
  virtual void pump() = 0;

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
