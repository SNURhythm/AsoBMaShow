#pragma once

#include "IInputBackend.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

class InputDeviceRegistry {
public:
  using BackendFactory =
      std::function<std::unique_ptr<IInputBackend>(input::InputBackendSink)>;
  using InputListener =
      std::function<void(const input::PhysicalInputEvent &event)>;
  using DeviceListener =
      std::function<void(const input::InputDeviceSnapshot &device)>;

  InputDeviceRegistry();
  explicit InputDeviceRegistry(std::vector<BackendFactory> backendFactories);
  ~InputDeviceRegistry();

  InputDeviceRegistry(const InputDeviceRegistry &) = delete;
  InputDeviceRegistry &operator=(const InputDeviceRegistry &) = delete;

  void handleSdlEvent(const SDL_Event &event);
  void pump();

  std::uint64_t subscribeInput(InputListener listener);
  std::uint64_t subscribeDevices(DeviceListener listener);
  void unsubscribe(std::uint64_t token);

  [[nodiscard]] std::vector<input::InputDeviceSnapshot> snapshot() const;
  [[nodiscard]] bool isConnected(std::string_view stableId) const;
  [[nodiscard]] const std::vector<std::string> &diagnostics() const;

private:
  using QueuedEvent =
      std::variant<input::PhysicalInputEvent, input::InputDeviceSnapshot>;

  void enqueueInput(input::PhysicalInputEvent event);
  void enqueueDevice(input::InputDeviceSnapshot device);

  mutable std::mutex queueMutex_;
  std::deque<QueuedEvent> queue_;
  std::vector<std::unique_ptr<IInputBackend>> backends_;
  std::unordered_map<std::string, input::InputDeviceSnapshot> devices_;
  std::unordered_map<std::uint64_t, InputListener> inputListeners_;
  std::unordered_map<std::uint64_t, DeviceListener> deviceListeners_;
  std::vector<std::string> diagnostics_;
  std::uint64_t nextSubscriptionToken_ = 1;
};
