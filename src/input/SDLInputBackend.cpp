#include "SDLInputBackend.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <unordered_set>
#include <utility>

namespace {

std::string copySdlString(const char *value) {
  return value == nullptr ? std::string{} : std::string(value);
}

std::string joystickGuid(SDL_Joystick *joystick) {
  std::array<char, 33> buffer{};
  SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), buffer.data(),
                            static_cast<int>(buffer.size()));
  return buffer.data();
}

class SdlInputDeviceProvider final : public ISdlInputDeviceProvider {
public:
  ~SdlInputDeviceProvider() override {
    for (auto &[instanceId, device] : openDevices_) {
      (void)instanceId;
      if (device.controller != nullptr) {
        SDL_GameControllerClose(device.controller);
      } else if (device.joystick != nullptr) {
        SDL_JoystickClose(device.joystick);
      }
    }
  }

  int deviceCount() const override { return SDL_NumJoysticks(); }

  bool isGameController(int deviceIndex) const override {
    return SDL_IsGameController(deviceIndex) == SDL_TRUE;
  }

  std::optional<SdlInputDeviceInfo>
  openDevice(int deviceIndex, bool asGameController,
             std::string &errorMessage) override {
    SDL_GameController *controller = nullptr;
    SDL_Joystick *joystick = nullptr;
    if (asGameController) {
      controller = SDL_GameControllerOpen(deviceIndex);
      if (controller != nullptr) {
        joystick = SDL_GameControllerGetJoystick(controller);
      }
    } else {
      joystick = SDL_JoystickOpen(deviceIndex);
    }

    if (joystick == nullptr) {
      errorMessage = copySdlString(SDL_GetError());
      if (controller != nullptr) {
        SDL_GameControllerClose(controller);
      }
      return std::nullopt;
    }

    const SDL_JoystickID instanceId = SDL_JoystickInstanceID(joystick);
    if (instanceId < 0) {
      errorMessage = copySdlString(SDL_GetError());
      if (controller != nullptr) {
        SDL_GameControllerClose(controller);
      } else {
        SDL_JoystickClose(joystick);
      }
      return std::nullopt;
    }
    if (openDevices_.contains(instanceId)) {
      errorMessage = "SDL input device instance is already open";
      if (controller != nullptr) {
        SDL_GameControllerClose(controller);
      } else {
        SDL_JoystickClose(joystick);
      }
      return std::nullopt;
    }

    const char *name = controller != nullptr
                           ? SDL_GameControllerName(controller)
                           : SDL_JoystickName(joystick);
    const char *serial = controller != nullptr
                             ? SDL_GameControllerGetSerial(controller)
                             : SDL_JoystickGetSerial(joystick);
    const char *path = controller != nullptr
                           ? SDL_GameControllerPath(controller)
                           : SDL_JoystickPath(joystick);
    SdlInputDeviceInfo result{
        .instanceId = instanceId,
        .gameController = asGameController,
        .guid = joystickGuid(joystick),
        .serial = copySdlString(serial),
        .path = copySdlString(path),
        .name = copySdlString(name),
        .buttons = std::max(0, SDL_JoystickNumButtons(joystick)),
        .axes = std::max(0, SDL_JoystickNumAxes(joystick)),
        .hats = std::max(0, SDL_JoystickNumHats(joystick))};
    openDevices_.emplace(
        instanceId, OpenDevice{.controller = controller, .joystick = joystick});
    return result;
  }

  void closeDevice(SDL_JoystickID instanceId) override {
    const auto found = openDevices_.find(instanceId);
    if (found == openDevices_.end()) {
      return;
    }
    if (found->second.controller != nullptr) {
      SDL_GameControllerClose(found->second.controller);
    } else if (found->second.joystick != nullptr) {
      SDL_JoystickClose(found->second.joystick);
    }
    openDevices_.erase(found);
  }

private:
  struct OpenDevice {
    SDL_GameController *controller = nullptr;
    SDL_Joystick *joystick = nullptr;
  };

  std::unordered_map<SDL_JoystickID, OpenDevice> openDevices_;
};

std::uint64_t toMicros(std::uint32_t timestamp) {
  return static_cast<std::uint64_t>(timestamp) * 1000U;
}

float normalizeAxis(Sint16 value) {
  return value < 0 ? static_cast<float>(value) / 32768.0F
                   : static_cast<float>(value) / 32767.0F;
}

} // namespace

SDLInputBackend::SDLInputBackend(
    input::InputBackendSink sink,
    std::shared_ptr<ISdlInputDeviceProvider> provider)
    : IInputBackend(std::move(sink)), provider_(std::move(provider)) {
  if (!provider_) {
    provider_ = std::make_shared<SdlInputDeviceProvider>();
  }
}

bool SDLInputBackend::start(std::string &errorMessage) {
  if (started_) {
    return true;
  }

  SDL_JoystickEventState(SDL_ENABLE);
  SDL_GameControllerEventState(SDL_ENABLE);
  const int count = provider_->deviceCount();
  if (count < 0) {
    std::string enumerationError = copySdlString(SDL_GetError());
    if (enumerationError.empty()) {
      enumerationError = "unknown error";
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                "SDL input enumeration failed; keyboard and hotplug remain "
                "active: %s",
                enumerationError.c_str());
    errorMessage.clear();
    started_ = true;
    return true;
  }

  started_ = true;
  std::vector<SdlInputDeviceInfo> openedDevices;
  std::unordered_set<SDL_JoystickID> openedInstanceIds;
  for (int deviceIndex = 0; deviceIndex < count; ++deviceIndex) {
    auto info = openDevice(deviceIndex);
    if (!info) {
      continue;
    }
    if (!openedInstanceIds.insert(info->instanceId).second) {
      provider_->closeDevice(info->instanceId);
      SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                  "Ignoring duplicate SDL input instance %d",
                  static_cast<int>(info->instanceId));
      continue;
    }
    openedDevices.push_back(std::move(*info));
  }

  std::vector<SdlDeviceIdentityDescriptor> descriptors;
  descriptors.reserve(openedDevices.size());
  for (const auto &info : openedDevices) {
    descriptors.push_back({.guid = info.guid,
                           .serial = info.serial,
                           .path = info.path,
                           .name = info.name});
  }
  const auto stableIds = identity_.connectBatch(descriptors);
  std::unordered_set<std::string> publishedStableIds;
  for (std::size_t index = 0; index < openedDevices.size(); ++index) {
    const bool publishConnection =
        publishedStableIds.insert(stableIds[index]).second;
    registerDevice(std::move(openedDevices[index]), stableIds[index],
                   publishConnection);
  }
  return true;
}

void SDLInputBackend::stop() {
  if (!started_ && devices_.empty()) {
    return;
  }
  for (const auto &[instanceId, device] : devices_) {
    identity_.disconnect(device.snapshot.stableId);
    provider_->closeDevice(instanceId);
  }
  devices_.clear();
  started_ = false;
}

void SDLInputBackend::handleSdlEvent(const SDL_Event &event) {
  switch (event.type) {
  case SDL_KEYDOWN:
  case SDL_KEYUP:
    if (event.type == SDL_KEYDOWN && event.key.repeat != 0) {
      return;
    }
    publishInput(
        {.control = {.deviceId = "keyboard",
                     .deviceClass = input::DeviceClass::Keyboard,
                     .kind = input::ControlKind::Key,
                     .index = static_cast<int>(event.key.keysym.scancode)},
         .rawValue = event.type == SDL_KEYDOWN ? 1.0 : 0.0,
         .normalizedValue = event.type == SDL_KEYDOWN ? 1.0F : 0.0F,
         .timestampMicros = toMicros(event.key.timestamp)});
    return;

  case SDL_JOYDEVICEADDED:
    addDevice(event.jdevice.which);
    return;
  case SDL_JOYDEVICEREMOVED:
    removeDevice(event.jdevice.which);
    return;

  case SDL_CONTROLLERBUTTONDOWN:
  case SDL_CONTROLLERBUTTONUP: {
    const auto found = devices_.find(event.cbutton.which);
    if (found != devices_.end() && found->second.gameController) {
      publishButton(found->second, event.cbutton.button,
                    event.type == SDL_CONTROLLERBUTTONDOWN,
                    event.cbutton.timestamp);
    }
    return;
  }
  case SDL_CONTROLLERAXISMOTION: {
    const auto found = devices_.find(event.caxis.which);
    if (found != devices_.end() && found->second.gameController) {
      publishAxis(found->second, event.caxis.axis, event.caxis.value,
                  event.caxis.timestamp);
    }
    return;
  }

  case SDL_JOYBUTTONDOWN:
  case SDL_JOYBUTTONUP: {
    const auto found = devices_.find(event.jbutton.which);
    if (found != devices_.end() && !found->second.gameController) {
      publishButton(found->second, event.jbutton.button,
                    event.type == SDL_JOYBUTTONDOWN, event.jbutton.timestamp);
    }
    return;
  }
  case SDL_JOYAXISMOTION: {
    const auto found = devices_.find(event.jaxis.which);
    if (found != devices_.end() && !found->second.gameController) {
      publishAxis(found->second, event.jaxis.axis, event.jaxis.value,
                  event.jaxis.timestamp);
    }
    return;
  }
  case SDL_JOYHATMOTION: {
    const auto found = devices_.find(event.jhat.which);
    if (found != devices_.end() && !found->second.gameController) {
      publishHat(found->second, event.jhat.hat, event.jhat.value,
                 event.jhat.timestamp);
    }
    return;
  }

  // SDL emits joystick lifecycle events for controller devices as well. Using
  // only that lifecycle prevents opening and publishing each controller twice.
  case SDL_CONTROLLERDEVICEADDED:
  case SDL_CONTROLLERDEVICEREMOVED:
  case SDL_CONTROLLERDEVICEREMAPPED:
  default:
    return;
  }
}

void SDLInputBackend::pump() {}

std::optional<SdlInputDeviceInfo> SDLInputBackend::openDevice(int deviceIndex) {
  const bool gameController = provider_->isGameController(deviceIndex);
  std::string errorMessage;
  auto info = provider_->openDevice(deviceIndex, gameController, errorMessage);
  if (!info) {
    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                "Could not open SDL input device %d: %s", deviceIndex,
                errorMessage.empty() ? "unknown error" : errorMessage.c_str());
    return std::nullopt;
  }
  return info;
}

void SDLInputBackend::registerDevice(SdlInputDeviceInfo info,
                                     std::string stableId,
                                     bool publishConnection) {
  const input::DeviceClass deviceClass =
      info.gameController ? input::DeviceClass::GameController
                          : input::DeviceClass::Joystick;
  const int advertisedButtons =
      info.gameController ? SDL_CONTROLLER_BUTTON_MAX : info.buttons;
  const int advertisedAxes =
      info.gameController ? SDL_CONTROLLER_AXIS_MAX : info.axes;
  const int advertisedHats = info.gameController ? 0 : info.hats;
  DeviceRecord record{
      .snapshot = {.stableId = std::move(stableId),
                   .displayName = info.name.empty()
                                      ? (info.gameController ? "Game Controller"
                                                             : "Joystick")
                                      : std::move(info.name),
                   .deviceClass = deviceClass,
                   .connected = true,
                   .buttons = advertisedButtons,
                   .axes = advertisedAxes,
                   .hats = advertisedHats},
      .gameController = info.gameController,
      .hatValues = std::vector<Uint8>(
          static_cast<std::size_t>(std::max(0, advertisedHats)),
          SDL_HAT_CENTERED)};
  if (publishConnection) {
    publishDevice(record.snapshot);
  }
  devices_.emplace(info.instanceId, std::move(record));
}

void SDLInputBackend::applyIdentityRemaps(
    std::span<const InputDeviceIdentityRemap> remappings) {
  for (const auto &remapping : remappings) {
    std::optional<input::InputDeviceSnapshot> oldSnapshot;
    std::optional<input::InputDeviceSnapshot> newSnapshot;
    for (auto &[instanceId, device] : devices_) {
      (void)instanceId;
      if (device.snapshot.stableId != remapping.fromStableId) {
        continue;
      }
      if (!oldSnapshot) {
        oldSnapshot = device.snapshot;
        oldSnapshot->connected = false;
      }
      device.snapshot.stableId = remapping.toStableId;
      if (!newSnapshot) {
        newSnapshot = device.snapshot;
      }
    }
    if (oldSnapshot && newSnapshot) {
      publishDevice(std::move(*oldSnapshot));
      publishDevice(std::move(*newSnapshot));
    }
  }
}

void SDLInputBackend::addDevice(int deviceIndex) {
  auto info = openDevice(deviceIndex);
  if (!info) {
    return;
  }
  if (devices_.contains(info->instanceId)) {
    provider_->closeDevice(info->instanceId);
    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                "Ignoring duplicate SDL input instance %d",
                static_cast<int>(info->instanceId));
    return;
  }

  const std::string stableId = identity_.connect({.guid = info->guid,
                                                  .serial = info->serial,
                                                  .path = info->path,
                                                  .name = info->name});
  applyIdentityRemaps(identity_.takeRemappings());
  const bool publishConnection = identity_.activeOwnerCount(stableId) == 1;
  registerDevice(std::move(*info), stableId, publishConnection);
}

void SDLInputBackend::removeDevice(SDL_JoystickID instanceId) {
  const auto found = devices_.find(instanceId);
  if (found == devices_.end()) {
    return;
  }

  found->second.snapshot.connected = false;
  if (identity_.disconnect(found->second.snapshot.stableId)) {
    publishDevice(found->second.snapshot);
  }
  provider_->closeDevice(instanceId);
  devices_.erase(found);
}

void SDLInputBackend::publishButton(const DeviceRecord &device, int button,
                                    bool pressed, std::uint32_t timestamp) {
  publishInput({.control = {.deviceId = device.snapshot.stableId,
                            .deviceClass = device.snapshot.deviceClass,
                            .kind = input::ControlKind::Button,
                            .index = button},
                .rawValue = pressed ? 1.0 : 0.0,
                .normalizedValue = pressed ? 1.0F : 0.0F,
                .timestampMicros = toMicros(timestamp)});
}

void SDLInputBackend::publishAxis(const DeviceRecord &device, int axis,
                                  Sint16 value, std::uint32_t timestamp) {
  publishInput({.control = {.deviceId = device.snapshot.stableId,
                            .deviceClass = device.snapshot.deviceClass,
                            .kind = input::ControlKind::Axis,
                            .index = axis},
                .rawValue = static_cast<double>(value),
                .normalizedValue = normalizeAxis(value),
                .timestampMicros = toMicros(timestamp)});
}

void SDLInputBackend::publishHat(DeviceRecord &device, int hat, Uint8 value,
                                 std::uint32_t timestamp) {
  if (hat < 0 || static_cast<std::size_t>(hat) >= device.hatValues.size()) {
    return;
  }

  const Uint8 previous = device.hatValues[static_cast<std::size_t>(hat)];
  device.hatValues[static_cast<std::size_t>(hat)] = value;
  constexpr std::array directions{
      std::pair{SDL_HAT_UP, input::ControlDirection::Up},
      std::pair{SDL_HAT_RIGHT, input::ControlDirection::Right},
      std::pair{SDL_HAT_DOWN, input::ControlDirection::Down},
      std::pair{SDL_HAT_LEFT, input::ControlDirection::Left}};
  for (const auto &[mask, direction] : directions) {
    const bool wasPressed = (previous & mask) != 0;
    const bool pressed = (value & mask) != 0;
    if (wasPressed == pressed) {
      continue;
    }
    publishInput({.control = {.deviceId = device.snapshot.stableId,
                              .deviceClass = device.snapshot.deviceClass,
                              .kind = input::ControlKind::Hat,
                              .index = hat,
                              .direction = direction},
                  .rawValue = pressed ? 1.0 : 0.0,
                  .normalizedValue = pressed ? 1.0F : 0.0F,
                  .timestampMicros = toMicros(timestamp)});
  }
}
