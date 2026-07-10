#include "InputDeviceRegistry.h"

#include "SDLInputBackend.h"

#include <SDL2/SDL_log.h>

#include <algorithm>
#include <utility>

namespace {

input::InputDeviceSnapshot keyboardSnapshot() {
  return {.stableId = "keyboard",
          .displayName = "Keyboard",
          .deviceClass = input::DeviceClass::Keyboard,
          .connected = true};
}

} // namespace

InputDeviceRegistry::InputDeviceRegistry()
    : InputDeviceRegistry({[](input::InputBackendSink sink) {
        return std::make_unique<SDLInputBackend>(std::move(sink));
      }}) {}

InputDeviceRegistry::InputDeviceRegistry(
    std::vector<BackendFactory> backendFactories) {
  const auto keyboard = keyboardSnapshot();
  devices_.emplace(keyboard.stableId, keyboard);
  enqueueDevice(keyboard);

  input::InputBackendSink sink{.enqueueInput =
                                   [this](input::PhysicalInputEvent event) {
                                     enqueueInput(std::move(event));
                                   },
                               .enqueueDevice =
                                   [this](input::InputDeviceSnapshot device) {
                                     enqueueDevice(std::move(device));
                                   }};

  backends_.reserve(backendFactories.size());
  for (auto &factory : backendFactories) {
    auto backend = factory(sink);
    if (!backend) {
      const std::string message = "Input backend factory returned null";
      diagnostics_.push_back(message);
      SDL_LogError(SDL_LOG_CATEGORY_INPUT, "%s", message.c_str());
      continue;
    }

    std::string errorMessage;
    if (!backend->start(errorMessage)) {
      if (errorMessage.empty()) {
        errorMessage = "unknown error";
      }
      const std::string message = "Input backend start failed: " + errorMessage;
      diagnostics_.push_back(message);
      SDL_LogError(SDL_LOG_CATEGORY_INPUT, "%s", message.c_str());
    }
    backends_.push_back(std::move(backend));
  }
}

InputDeviceRegistry::~InputDeviceRegistry() {
  for (auto backend = backends_.rbegin(); backend != backends_.rend();
       ++backend) {
    (*backend)->stop();
  }
}

void InputDeviceRegistry::handleSdlEvent(const SDL_Event &event) {
  for (const auto &backend : backends_) {
    backend->handleSdlEvent(event);
  }
}

void InputDeviceRegistry::pump() {
  for (const auto &backend : backends_) {
    backend->pump();
  }

  std::deque<QueuedEvent> pending;
  {
    const std::lock_guard lock(queueMutex_);
    pending.swap(queue_);
  }

  for (const auto &event : pending) {
    if (const auto *inputEvent =
            std::get_if<input::PhysicalInputEvent>(&event)) {
      std::vector<InputListener> listeners;
      listeners.reserve(inputListeners_.size());
      for (const auto &[token, listener] : inputListeners_) {
        (void)token;
        listeners.push_back(listener);
      }
      for (const auto &listener : listeners) {
        listener(*inputEvent);
      }
      continue;
    }

    const auto &device = std::get<input::InputDeviceSnapshot>(event);
    devices_.insert_or_assign(device.stableId, device);
    std::vector<DeviceListener> listeners;
    listeners.reserve(deviceListeners_.size());
    for (const auto &[token, listener] : deviceListeners_) {
      (void)token;
      listeners.push_back(listener);
    }
    for (const auto &listener : listeners) {
      listener(device);
    }
  }
}

std::uint64_t InputDeviceRegistry::subscribeInput(InputListener listener) {
  const std::uint64_t token = nextSubscriptionToken_++;
  inputListeners_.emplace(token, std::move(listener));
  return token;
}

std::uint64_t InputDeviceRegistry::subscribeDevices(DeviceListener listener) {
  const std::uint64_t token = nextSubscriptionToken_++;
  deviceListeners_.emplace(token, std::move(listener));
  return token;
}

void InputDeviceRegistry::unsubscribe(std::uint64_t token) {
  inputListeners_.erase(token);
  deviceListeners_.erase(token);
}

std::vector<input::InputDeviceSnapshot> InputDeviceRegistry::snapshot() const {
  std::vector<input::InputDeviceSnapshot> result;
  result.reserve(devices_.size());
  for (const auto &[stableId, device] : devices_) {
    (void)stableId;
    result.push_back(device);
  }
  std::ranges::sort(result, {}, &input::InputDeviceSnapshot::stableId);
  return result;
}

bool InputDeviceRegistry::isConnected(std::string_view stableId) const {
  const auto device = devices_.find(std::string(stableId));
  return device != devices_.end() && device->second.connected;
}

const std::vector<std::string> &InputDeviceRegistry::diagnostics() const {
  return diagnostics_;
}

void InputDeviceRegistry::enqueueInput(input::PhysicalInputEvent event) {
  const std::lock_guard lock(queueMutex_);
  queue_.emplace_back(std::move(event));
}

void InputDeviceRegistry::enqueueDevice(input::InputDeviceSnapshot device) {
  const std::lock_guard lock(queueMutex_);
  queue_.emplace_back(std::move(device));
}
