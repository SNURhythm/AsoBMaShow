#pragma once

#include "IInputBackend.h"
#include "InputDeviceIdentity.h"

#include <SDL2/SDL_joystick.h>

#include <memory>
#include <optional>
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
  int buttons = 0;
  int axes = 0;
  int hats = 0;
};

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
      std::shared_ptr<ISdlInputDeviceProvider> provider = {});

  bool start(std::string &errorMessage) override;
  void stop() override;
  void handleSdlEvent(const SDL_Event &event) override;
  void pump() override;

private:
  struct DeviceRecord {
    input::InputDeviceSnapshot snapshot;
    bool gameController = false;
    std::vector<Uint8> hatValues;
  };

  void addDevice(int deviceIndex);
  void removeDevice(SDL_JoystickID instanceId);
  void publishButton(const DeviceRecord &device, int button, bool pressed,
                     std::uint32_t timestamp);
  void publishAxis(const DeviceRecord &device, int axis, Sint16 value,
                   std::uint32_t timestamp);
  void publishHat(DeviceRecord &device, int hat, Uint8 value,
                  std::uint32_t timestamp);

  std::shared_ptr<ISdlInputDeviceProvider> provider_;
  InputDeviceIdentity identity_;
  std::unordered_map<SDL_JoystickID, DeviceRecord> devices_;
  bool started_ = false;
};
