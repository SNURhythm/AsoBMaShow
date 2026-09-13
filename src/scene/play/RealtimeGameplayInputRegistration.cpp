#include "RealtimeGameplayInputRegistration.h"

#include <utility>

namespace gameplay {

RealtimeGameplayInputRegistration::RealtimeGameplayInputRegistration(
    InputDeviceRegistry &registry, std::atomic_bool &acceptingNativeInput,
    Configuration configuration)
    : registry_(registry), acceptingNativeInput_(acceptingNativeInput),
      configuration_(std::move(configuration)) {
  acceptingNativeInput_.store(false, std::memory_order_release);
  try {
    inputSubscription_ = registry_.subscribeRealtimeInput([this](const auto &event) {
      if (acceptingNativeInput_.load(std::memory_order_acquire) && configuration_.onInput) {
        configuration_.onInput(event);
      }
    });
    deviceSubscription_ = registry_.subscribeRealtimeDevices([this](const auto &device) {
      if (acceptingNativeInput_.load(std::memory_order_acquire) && configuration_.onDevice) {
        configuration_.onDevice(device);
      }
    });
    for (std::size_t index = 0; index < configuration_.claimedClasses.size(); ++index) {
      if (configuration_.claimedClasses[index]) {
        disabledLegacyClasses_[index] = true;
        configuration_.setLegacyClassEnabled(static_cast<input::DeviceClass>(index), false);
      }
    }
    if (configuration_.sdlWatch != nullptr) {
      SDL_AddEventWatch(&watch, this);
      watchingSdl_ = true;
    }
  } catch (...) {
    close();
    throw;
  }
}

RealtimeGameplayInputRegistration::~RealtimeGameplayInputRegistration() {
  close();
}

int SDLCALL RealtimeGameplayInputRegistration::watch(void *context, SDL_Event *event) {
  auto &registration = *static_cast<RealtimeGameplayInputRegistration *>(context);
  if (!registration.acceptingNativeInput_.load(std::memory_order_acquire)) return 0;
  return registration.configuration_.sdlWatch(registration.configuration_.sdlWatchContext, event);
}

bool RealtimeGameplayInputRegistration::activate() {
  if (closed_) return false;
  if (activated_) return true;
  activated_ = true;
  acceptingNativeInput_.store(true, std::memory_order_release);
  try {
    for (std::size_t index = 0; index < configuration_.claimedClasses.size(); ++index) {
      if (configuration_.claimedClasses[index]) {
        registry_.setRealtimeInputClaimed(static_cast<input::DeviceClass>(index), true);
      }
    }
  } catch (...) {
    close();
    throw;
  }
  return true;
}

void RealtimeGameplayInputRegistration::close() {
  if (closed_) return;
  closed_ = true;
  acceptingNativeInput_.store(false, std::memory_order_release);
  if (watchingSdl_) {
    SDL_DelEventWatch(&watch, this);
    watchingSdl_ = false;
  }
  if (inputSubscription_ != 0) {
    registry_.unsubscribe(std::exchange(inputSubscription_, 0));
  }
  if (deviceSubscription_ != 0) {
    registry_.unsubscribe(std::exchange(deviceSubscription_, 0));
  }
  for (std::size_t index = 0; index < disabledLegacyClasses_.size(); ++index) {
    if (disabledLegacyClasses_[index]) {
      const auto deviceClass = static_cast<input::DeviceClass>(index);
      registry_.setRealtimeInputClaimed(deviceClass, false);
      configuration_.setLegacyClassEnabled(deviceClass, true);
      disabledLegacyClasses_[index] = false;
    }
  }
}

} // namespace gameplay
