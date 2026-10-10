#pragma once

#include "IInputBackend.h"
#include "InputDeviceIdentity.h"
#include "RealtimeControllerDeviceMap.h"

#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_scancode.h>

#include <array>
#include <atomic>
#include <bitset>
#include <compare>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct SdlInputDeviceInfo {
  SDL_JoystickID instanceId = 0;
  bool gameController = false;
  std::string guid;
  std::string serial;
  std::string path;
  std::string name;
  std::string legacyName;
  int buttons = 0;
  int axes = 0;
  int hats = 0;
  int playerIndex = -1;
  std::vector<int> pressedRawButtons;
};

struct SdlGdxKeyAlias {
  SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
  int gdxKeyCode = -1;
  auto operator<=>(const SdlGdxKeyAlias &) const = default;
};

[[nodiscard]] std::span<const SdlGdxKeyAlias> sdlGdxKeyAliases() noexcept;
[[nodiscard]] int gdxKeyCodeForSdlScancode(SDL_Scancode) noexcept;

class ISdlInputDeviceProvider {
public:
  virtual ~ISdlInputDeviceProvider() = default;

  [[nodiscard]] virtual std::optional<std::vector<SDL_JoystickID>> deviceIds() const = 0;
  [[nodiscard]] virtual bool isGameController(SDL_JoystickID deviceId) const = 0;
  virtual std::optional<SdlInputDeviceInfo>
  openDevice(SDL_JoystickID deviceId, bool asGameController,
             std::string &errorMessage) = 0;
  virtual void closeDevice(SDL_JoystickID instanceId) = 0;
};

class SDLInputBackend final : public IInputBackend {
public:
  explicit SDLInputBackend(
      input::InputBackendSink sink,
      std::shared_ptr<ISdlInputDeviceProvider> provider = {},
      std::shared_ptr<RealtimeControllerDeviceMap> realtimeControllerMap = {});

  bool start(std::string &errorMessage) override;
  void stop() override;
  void handleSdlEvent(const SDL_Event &event) override;
  void pump() override;
  void reconcileDevices();
  void setRealtimeInputClaimed(input::DeviceClass deviceClass,
                               bool claimed) override;
  [[nodiscard]] std::optional<input::PhysicalInputEvent>
  translateRealtimeInput(const SDL_Event &event) const;
  std::size_t translateRealtimeInputs(
      const SDL_Event &event,
      std::span<input::PhysicalInputEvent> output, bool consumeOnce = false);
  [[nodiscard]] std::optional<std::string>
  realtimeDisconnectedDeviceId(const SDL_Event &event) const;
  [[nodiscard]] input::LegacyInputGeneration
  legacyControllerGeneration() const noexcept;

private:
  using RealtimeEventKey = std::array<std::uint64_t, 5>;
  struct RealtimeDelivery {
    RealtimeEventKey key;
    bool delivered = false;
  };
  static std::optional<RealtimeEventKey> realtimeEventKey(const SDL_Event &);
  std::size_t translateRealtimeInputsUnclaimed(
      const SDL_Event &, std::span<input::PhysicalInputEvent>);

  struct DeviceRecord {
    input::InputDeviceSnapshot snapshot;
    bool gameController = false;
    int playerIndex = -1;
    std::string legacyName;
    std::bitset<input::kLegacyInputMaximumButtons> pressedRawButtons;
    std::uint64_t legacyOrder = 0;
    std::vector<Uint8> hatValues;
  };

  std::optional<SdlInputDeviceInfo> openDevice(SDL_JoystickID deviceId);
  void registerDevice(SdlInputDeviceInfo info, std::string stableId,
                      bool publishConnection);
  void
  applyIdentityRemaps(std::span<const InputDeviceIdentityRemap> remappings);
  void addDevice(SDL_JoystickID deviceId);
  void removeDevice(SDL_JoystickID instanceId);
  void publishButton(const DeviceRecord &device, int button, bool pressed,
                     std::uint64_t timestamp);
  void publishAxis(const DeviceRecord &device, int axis, Sint16 value,
                   std::uint64_t timestamp);
  void publishHat(DeviceRecord &device, int hat, Uint8 value,
                  std::uint64_t timestamp);
  void rebuildLegacyControllerGenerationLocked() noexcept;
  [[nodiscard]] bool
  nativeRealtimeOwns(input::DeviceClass deviceClass) const noexcept;

  std::shared_ptr<ISdlInputDeviceProvider> provider_;
  std::shared_ptr<RealtimeControllerDeviceMap> realtimeControllerMap_;
  InputDeviceIdentity identity_;
  std::unordered_map<SDL_JoystickID, DeviceRecord> devices_;
  input::LegacyInputGeneration legacyControllerGeneration_;
  std::uint64_t nextLegacyOrder_ = 1;
  mutable std::mutex devicesMutex_;
  // Held through queued publication so a newly mapped source cannot overtake
  // its deferred hotplug edges. Never acquire this while holding devicesMutex_.
  std::mutex realtimeDeliveryMutex_;
  std::deque<RealtimeDelivery> realtimeDeliveries_;
  std::unordered_map<SDL_JoystickID, std::size_t> pendingRealtimeInputs_;
  bool realtimeDeliveryOverflow_ = false;
  std::array<std::atomic_bool, 6> realtimeInputClaimed_{};
  bool started_ = false;
};
