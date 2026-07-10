#pragma once

#include "IInputBackend.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
  // Dispatches this SDL event before returning without polling async backends.
  void handleSdlEventAndDispatch(const SDL_Event &event);
  void pump();

  // Input subscriptions never inherit events queued before their sequence
  // boundary.
  std::uint64_t subscribeInput(InputListener listener);
  std::uint64_t subscribeDevices(DeviceListener listener);
  // Removing a subscription cancels any events not yet delivered to that
  // listener.
  void unsubscribe(std::uint64_t token);

  [[nodiscard]] std::vector<input::InputDeviceSnapshot> snapshot() const;
  [[nodiscard]] bool isConnected(std::string_view stableId) const;
  [[nodiscard]] const std::vector<std::string> &diagnostics() const;

private:
  using QueuedPayload =
      std::variant<input::PhysicalInputEvent, input::InputDeviceSnapshot>;
  struct QueuedEvent {
    std::uint64_t sequence = 0;
    QueuedPayload payload;
  };
  struct InputSubscription {
    std::uint64_t firstEventSequence = 0;
    InputListener listener;
  };
  struct QueueState;
  struct BackendSinkGate;

  void enqueueInput(input::PhysicalInputEvent event);
  void enqueueDevice(input::InputDeviceSnapshot device);
  void dispatchPending();
  void pumpInternal(bool pumpBackends);

  std::shared_ptr<QueueState> queueState_;
  std::vector<std::unique_ptr<IInputBackend>> backends_;
  std::vector<std::shared_ptr<BackendSinkGate>> backendSinkGates_;
  std::unordered_map<std::string, input::InputDeviceSnapshot> devices_;
  std::map<std::uint64_t, InputSubscription> inputListeners_;
  std::map<std::uint64_t, DeviceListener> deviceListeners_;
  std::vector<std::string> diagnostics_;
  std::uint64_t nextSubscriptionToken_ = 1;
  bool pumping_ = false;
  bool repumpRequested_ = false;
  bool repumpBackendsRequested_ = false;
};
