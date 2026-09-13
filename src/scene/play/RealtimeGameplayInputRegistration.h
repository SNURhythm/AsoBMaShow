#pragma once

#include "../../input/InputDeviceRegistry.h"

#include <array>
#include <atomic>
#include <functional>

namespace gameplay {

// Owns native registrations and device-class routing for one gameplay session.
// Lifecycle calls are serialized by the scene; producers may invoke callbacks
// concurrently. The registry, acceptance flag, and callback dependencies must
// outlive this owner. Close before stopping the gameplay worker.
class RealtimeGameplayInputRegistration final {
public:
  using DeviceClasses = std::array<bool, 6>;
  struct Configuration {
    DeviceClasses claimedClasses{};
    // Restoring a class must not throw. The scene's legacy input handler owns
    // the routing state; this owner pairs each disable with one restoration.
    std::function<void(input::DeviceClass, bool)> setLegacyClassEnabled;
    InputDeviceRegistry::InputListener onInput;
    InputDeviceRegistry::DeviceListener onDevice;
    SDL_EventFilter sdlWatch = nullptr;
    void *sdlWatchContext = nullptr;
  };

  // Register callbacks and disable selected legacy classes, with native
  // delivery still gated. Partial setup is rolled back before rethrowing.
  RealtimeGameplayInputRegistration(InputDeviceRegistry &,
                                     std::atomic_bool &acceptingNativeInput,
                                     Configuration);
  ~RealtimeGameplayInputRegistration();
  RealtimeGameplayInputRegistration(const RealtimeGameplayInputRegistration &) = delete;
  RealtimeGameplayInputRegistration &operator=(const RealtimeGameplayInputRegistration &) = delete;

  // Call after scene ingress is ready. Repeated activation is harmless;
  // closed owners cannot reactivate. A new attempt needs a new owner.
  bool activate();
  // Gate delivery, detach callbacks (waiting for active registry callbacks),
  // then restore routing. Safe before activation and on repeated calls.
  void close();

private:
  static int SDLCALL watch(void *, SDL_Event *);

  InputDeviceRegistry &registry_;
  std::atomic_bool &acceptingNativeInput_;
  Configuration configuration_;
  DeviceClasses disabledLegacyClasses_{};
  std::uint64_t inputSubscription_ = 0;
  std::uint64_t deviceSubscription_ = 0;
  bool watchingSdl_ = false;
  bool activated_ = false;
  bool closed_ = false;
};

} // namespace gameplay
