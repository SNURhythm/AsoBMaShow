#include "SDLInputBackend.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace {

constexpr float kIosAccelerometerTiltAxisGain = 2.5F;

std::string copySdlString(const char *value) {
  return value == nullptr ? std::string{} : std::string(value);
}

std::string joystickGuid(SDL_Joystick *joystick) {
  std::array<char, 33> buffer{};
  SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), buffer.data(),
                            static_cast<int>(buffer.size()));
  return buffer.data();
}

int xinputPlayerIndex(std::string_view path) {
  constexpr std::string_view prefix = "XInput#";
  if (!path.starts_with(prefix)) {
    return -1;
  }
  int playerIndex = -1;
  const char *begin = path.data() + prefix.size();
  const char *end = path.data() + path.size();
  const auto parsed = std::from_chars(begin, end, playerIndex);
  return parsed.ec == std::errc{} && parsed.ptr == end && playerIndex >= 0 &&
                 playerIndex < RealtimeControllerDeviceMap::kMaxPlayers
             ? playerIndex
             : -1;
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
    const std::string pathValue = copySdlString(path);
    SdlInputDeviceInfo result{
        .instanceId = instanceId,
        .gameController = asGameController,
        .guid = joystickGuid(joystick),
        .serial = copySdlString(serial),
        .path = pathValue,
        .name = copySdlString(name),
        .buttons = std::max(0, SDL_JoystickNumButtons(joystick)),
        .axes = std::max(0, SDL_JoystickNumAxes(joystick)),
        .hats = std::max(0, SDL_JoystickNumHats(joystick)),
        .playerIndex =
            controller != nullptr ? xinputPlayerIndex(pathValue) : -1};
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
    std::shared_ptr<ISdlInputDeviceProvider> provider,
    std::shared_ptr<RealtimeControllerDeviceMap> realtimeControllerMap)
    : IInputBackend(std::move(sink)), provider_(std::move(provider)),
      realtimeControllerMap_(std::move(realtimeControllerMap)) {
  if (!provider_) {
    provider_ = std::make_shared<SdlInputDeviceProvider>();
  }
}

bool SDLInputBackend::start(std::string &errorMessage) {
  const std::lock_guard lock(devicesMutex_);
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
  const std::lock_guard lock(devicesMutex_);
  if (!started_ && devices_.empty()) {
    return;
  }
  for (const auto &[instanceId, device] : devices_) {
    identity_.disconnect(device.snapshot.stableId);
    if (realtimeControllerMap_ && device.gameController) {
      realtimeControllerMap_->clear(device.playerIndex,
                                    device.snapshot.stableId);
    }
    provider_->closeDevice(instanceId);
  }
  devices_.clear();
  started_ = false;
}

void SDLInputBackend::handleSdlEvent(const SDL_Event &event) {
  if (event.type == SDL_JOYDEVICEADDED) {
    addDevice(event.jdevice.which);
    return;
  }
  if (event.type == SDL_JOYDEVICEREMOVED) {
    removeDevice(event.jdevice.which);
    return;
  }
  const std::lock_guard lock(devicesMutex_);
  switch (event.type) {
  case SDL_KEYDOWN:
  case SDL_KEYUP:
    if (nativeRealtimeOwns(input::DeviceClass::Keyboard)) {
      return;
    }
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

  case SDL_CONTROLLERBUTTONDOWN:
  case SDL_CONTROLLERBUTTONUP: {
    const auto found = devices_.find(event.cbutton.which);
    if (found != devices_.end() && found->second.gameController) {
      if (found->second.playerIndex >= 0 &&
          nativeRealtimeOwns(input::DeviceClass::GameController)) {
        return;
      }
      publishButton(found->second, event.cbutton.button,
                    event.type == SDL_CONTROLLERBUTTONDOWN,
                    event.cbutton.timestamp);
    }
    return;
  }
  case SDL_CONTROLLERAXISMOTION: {
    const auto found = devices_.find(event.caxis.which);
    if (found != devices_.end() && found->second.gameController) {
      if (found->second.playerIndex >= 0 &&
          nativeRealtimeOwns(input::DeviceClass::GameController)) {
        return;
      }
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

void SDLInputBackend::setRealtimeInputClaimed(
    input::DeviceClass deviceClass, bool claimed) {
  const auto index = static_cast<std::size_t>(deviceClass);
  if (index < realtimeInputClaimed_.size()) {
    realtimeInputClaimed_[index].store(claimed, std::memory_order_release);
  }
}

bool SDLInputBackend::nativeRealtimeOwns(
    input::DeviceClass deviceClass) const noexcept {
  if (!realtimeControllerMap_) {
    return false;
  }
  const auto index = static_cast<std::size_t>(deviceClass);
  if (index >= realtimeInputClaimed_.size() ||
      !realtimeInputClaimed_[index].load(std::memory_order_acquire)) {
    return false;
  }
  if (deviceClass == input::DeviceClass::Keyboard) {
    return realtimeControllerMap_->keyboardRealtimeAvailable();
  }
  if (deviceClass == input::DeviceClass::GameController) {
    return realtimeControllerMap_->controllerRealtimeAvailable();
  }
  return false;
}

std::optional<input::PhysicalInputEvent>
SDLInputBackend::translateRealtimeInput(const SDL_Event &event) const {
  if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
    if (event.type == SDL_KEYDOWN && event.key.repeat != 0) {
      return std::nullopt;
    }
    return input::PhysicalInputEvent{
        .control = {.deviceId = "keyboard",
                    .deviceClass = input::DeviceClass::Keyboard,
                    .kind = input::ControlKind::Key,
                    .index = static_cast<int>(event.key.keysym.scancode)},
        .rawValue = event.type == SDL_KEYDOWN ? 1.0 : 0.0,
        .normalizedValue = event.type == SDL_KEYDOWN ? 1.0F : 0.0F,
        .timestampMicros = toMicros(event.key.timestamp)};
  }

  const std::lock_guard lock(devicesMutex_);
  switch (event.type) {
  case SDL_CONTROLLERBUTTONDOWN:
  case SDL_CONTROLLERBUTTONUP: {
    const auto found = devices_.find(event.cbutton.which);
    if (found == devices_.end() || !found->second.gameController) {
      return std::nullopt;
    }
    const bool pressed = event.type == SDL_CONTROLLERBUTTONDOWN;
    return input::PhysicalInputEvent{
        .control = {.deviceId = found->second.snapshot.stableId,
                    .deviceClass = input::DeviceClass::GameController,
                    .kind = input::ControlKind::Button,
                    .index = event.cbutton.button},
        .rawValue = pressed ? 1.0 : 0.0,
        .normalizedValue = pressed ? 1.0F : 0.0F,
        .timestampMicros = toMicros(event.cbutton.timestamp)};
  }
  case SDL_CONTROLLERAXISMOTION: {
    const auto found = devices_.find(event.caxis.which);
    if (found == devices_.end() || !found->second.gameController) {
      return std::nullopt;
    }
    const float value = normalizeAxis(event.caxis.value);
    return input::PhysicalInputEvent{
        .control = {.deviceId = found->second.snapshot.stableId,
                    .deviceClass = input::DeviceClass::GameController,
                    .kind = input::ControlKind::Axis,
                    .index = event.caxis.axis},
        .rawValue = static_cast<double>(event.caxis.value),
        .normalizedValue = value,
        .timestampMicros = toMicros(event.caxis.timestamp)};
  }
  case SDL_JOYBUTTONDOWN:
  case SDL_JOYBUTTONUP: {
    const auto found = devices_.find(event.jbutton.which);
    if (found == devices_.end() || found->second.gameController) {
      return std::nullopt;
    }
    const bool pressed = event.type == SDL_JOYBUTTONDOWN;
    return input::PhysicalInputEvent{
        .control = {.deviceId = found->second.snapshot.stableId,
                    .deviceClass = input::DeviceClass::Joystick,
                    .kind = input::ControlKind::Button,
                    .index = event.jbutton.button},
        .rawValue = pressed ? 1.0 : 0.0,
        .normalizedValue = pressed ? 1.0F : 0.0F,
        .timestampMicros = toMicros(event.jbutton.timestamp)};
  }
  case SDL_JOYAXISMOTION: {
    const auto found = devices_.find(event.jaxis.which);
    if (found == devices_.end() || found->second.gameController) {
      return std::nullopt;
    }
    float value = normalizeAxis(event.jaxis.value);
    if (found->second.iosAccelerometer &&
        (event.jaxis.axis == 0 || event.jaxis.axis == 1)) {
      value = std::clamp(value * kIosAccelerometerTiltAxisGain, -1.0F, 1.0F);
    }
    return input::PhysicalInputEvent{
        .control = {.deviceId = found->second.snapshot.stableId,
                    .deviceClass = input::DeviceClass::Joystick,
                    .kind = input::ControlKind::Axis,
                    .index = event.jaxis.axis},
        .rawValue = static_cast<double>(event.jaxis.value),
        .normalizedValue = value,
        .timestampMicros = toMicros(event.jaxis.timestamp)};
  }
  default:
    return std::nullopt;
  }
}

std::optional<std::string>
SDLInputBackend::realtimeDisconnectedDeviceId(const SDL_Event &event) const {
  SDL_JoystickID instanceId = -1;
  if (event.type == SDL_JOYDEVICEREMOVED) {
    instanceId = event.jdevice.which;
  } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
    instanceId = event.cdevice.which;
  } else {
    return std::nullopt;
  }
  const std::lock_guard lock(devicesMutex_);
  const auto device = devices_.find(instanceId);
  return device == devices_.end()
             ? std::nullopt
             : std::optional<std::string>{device->second.snapshot.stableId};
}

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
  const bool iosAccelerometer =
      !info.gameController && info.name == "iOS Accelerometer" &&
      info.buttons == 0 && info.axes == 3 && info.hats == 0;
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
      .iosAccelerometer = iosAccelerometer,
      .playerIndex = info.playerIndex,
      .hatValues = std::vector<Uint8>(
          static_cast<std::size_t>(std::max(0, advertisedHats)),
          SDL_HAT_CENTERED)};
  if (publishConnection) {
    publishDevice(record.snapshot);
  }
  if (realtimeControllerMap_ && record.gameController) {
    realtimeControllerMap_->assign(record.playerIndex,
                                   record.snapshot.stableId);
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
      if (realtimeControllerMap_ && device.gameController) {
        realtimeControllerMap_->assign(device.playerIndex,
                                       device.snapshot.stableId);
      }
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
  bool duplicate = false;
  {
    const std::lock_guard lock(devicesMutex_);
    duplicate = devices_.contains(info->instanceId);
    if (!duplicate) {
      const std::string stableId = identity_.connect({.guid = info->guid,
                                                      .serial = info->serial,
                                                      .path = info->path,
                                                      .name = info->name});
      applyIdentityRemaps(identity_.takeRemappings());
      const bool publishConnection =
          identity_.activeOwnerCount(stableId) == 1;
      registerDevice(std::move(*info), stableId, publishConnection);
    }
  }
  if (duplicate) {
    provider_->closeDevice(info->instanceId);
    SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                "Ignoring duplicate SDL input instance %d",
                static_cast<int>(info->instanceId));
  }
}

void SDLInputBackend::removeDevice(SDL_JoystickID instanceId) {
  std::optional<input::InputDeviceSnapshot> disconnected;
  {
    const std::lock_guard lock(devicesMutex_);
    const auto found = devices_.find(instanceId);
    if (found == devices_.end()) {
      return;
    }
    found->second.snapshot.connected = false;
    if (identity_.disconnect(found->second.snapshot.stableId)) {
      disconnected = found->second.snapshot;
    }
    if (realtimeControllerMap_ && found->second.gameController) {
      realtimeControllerMap_->clear(found->second.playerIndex,
                                    found->second.snapshot.stableId);
    }
    devices_.erase(found);
  }
  if (disconnected.has_value()) {
    publishDevice(std::move(*disconnected));
  }
  provider_->closeDevice(instanceId);
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
  float normalizedValue = normalizeAxis(value);
  if (device.iosAccelerometer && (axis == 0 || axis == 1)) {
    normalizedValue = std::clamp(normalizedValue *
                                     kIosAccelerometerTiltAxisGain,
                                 -1.0F, 1.0F);
  }
  publishInput({.control = {.deviceId = device.snapshot.stableId,
                            .deviceClass = device.snapshot.deviceClass,
                            .kind = input::ControlKind::Axis,
                            .index = axis},
                .rawValue = static_cast<double>(value),
                .normalizedValue = normalizedValue,
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
