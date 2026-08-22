#pragma once

#include "IInputBackend.h"

#include <cstdint>
#include <array>
#include <bitset>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include <SDL2/SDL_scancode.h>

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
  void configureGyroscopeTurntable(input::GyroscopeTurntableConfig config);
  void resetGyroscopeTurntableSession();
  // Claimed classes are delivered to realtime listeners but omitted from the
  // ordinary frame queue, preventing delayed duplicate gameplay edges.
  void setRealtimeInputClaimed(input::DeviceClass deviceClass, bool claimed);
  [[nodiscard]] std::optional<input::PhysicalInputEvent>
  translateRealtimeSdlInput(const SDL_Event &) const;
  std::size_t translateRealtimeSdlInputs(
      const SDL_Event &, std::span<input::PhysicalInputEvent> output);
  [[nodiscard]] std::optional<std::string>
  realtimeDisconnectedSdlDevice(const SDL_Event &) const;

  // Input subscriptions never inherit events queued before their sequence
  // boundary.
  std::uint64_t subscribeInput(InputListener listener);
  // Runs on the producing native callback thread before that input is queued
  // for ordinary frame dispatch. Unsubscribe waits for an active callback.
  std::uint64_t subscribeRealtimeInput(InputListener listener);
  std::uint64_t subscribeRealtimeDevices(DeviceListener listener);
  std::uint64_t subscribeDevices(DeviceListener listener);
  // Removing a subscription cancels any events not yet delivered to that
  // listener.
  void unsubscribe(std::uint64_t token);

  [[nodiscard]] std::vector<input::InputDeviceSnapshot> snapshot() const;
  [[nodiscard]] input::LegacyInputGeneration
  legacyInputGeneration(int drawableWidth, int drawableHeight) const noexcept;
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
  class SDLInputBackend *sdlInputBackend_ = nullptr;
  mutable std::mutex legacyInputMutex_;
  std::bitset<SDL_NUM_SCANCODES> pressedSdlScancodes_;
  std::bitset<input::kLegacyInputMaximumGdxKeyCode + 1> pressedGdxKeys_;
  std::array<std::uint16_t, input::kLegacyInputMaximumGdxKeyCode + 1>
      pressedGdxKeyCounts_{};
  mutable std::uint64_t legacyInputSequence_ = 0;
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
