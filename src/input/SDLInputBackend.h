#pragma once

#include "IInputBackend.h"
#include "InputDeviceIdentity.h"
#include "RealtimeControllerDeviceMap.h"

#include <SDL2/SDL_joystick.h>
#include <SDL2/SDL_scancode.h>

#include <array>
#include <atomic>
#include <bitset>
#include <compare>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct SdlInputDeviceInfo {
  SDL_JoystickID instanceId = -1;
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

  [[nodiscard]] virtual int deviceCount() const = 0;
  [[nodiscard]] virtual bool isGameController(int deviceIndex) const = 0;
  virtual std::optional<SdlInputDeviceInfo>
  openDevice(int deviceIndex, bool asGameController,
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
  void setRealtimeInputClaimed(input::DeviceClass deviceClass,
                               bool claimed) override;
  [[nodiscard]] std::optional<input::PhysicalInputEvent>
  translateRealtimeInput(const SDL_Event &event) const;
  std::size_t translateRealtimeInputs(
      const SDL_Event &event,
      std::span<input::PhysicalInputEvent> output);
  [[nodiscard]] std::optional<std::string>
  realtimeDisconnectedDeviceId(const SDL_Event &event) const;
  [[nodiscard]] input::LegacyInputGeneration
  legacyControllerGeneration() const noexcept;

private:
  struct DeviceRecord {
    input::InputDeviceSnapshot snapshot;
    bool gameController = false;
    bool iosAccelerometer = false;
    int playerIndex = -1;
    std::string legacyName;
    std::bitset<input::kLegacyInputMaximumButtons> pressedRawButtons;
    std::uint64_t legacyOrder = 0;
    std::vector<Uint8> hatValues;
  };

  std::optional<SdlInputDeviceInfo> openDevice(int deviceIndex);
  void registerDevice(SdlInputDeviceInfo info, std::string stableId,
                      bool publishConnection);
  void
  applyIdentityRemaps(std::span<const InputDeviceIdentityRemap> remappings);
  void addDevice(int deviceIndex);
  void removeDevice(SDL_JoystickID instanceId);
  void publishButton(const DeviceRecord &device, int button, bool pressed,
                     std::uint32_t timestamp);
  void publishAxis(const DeviceRecord &device, int axis, Sint16 value,
                   std::uint32_t timestamp);
  void publishHat(DeviceRecord &device, int hat, Uint8 value,
                  std::uint32_t timestamp);
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
  std::array<std::atomic_bool, 6> realtimeInputClaimed_{};
  bool started_ = false;
};
