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

constexpr auto kSdlGdxKeyAliases = std::to_array<SdlGdxKeyAlias>({
    {SDL_SCANCODE_SOFTLEFT, 1},       {SDL_SCANCODE_SOFTRIGHT, 2},
    {SDL_SCANCODE_HOME, 3},           {SDL_SCANCODE_AC_HOME, 3},
    {SDL_SCANCODE_AC_BACK, 4},        {SDL_SCANCODE_CALL, 5},
    {SDL_SCANCODE_ENDCALL, 6},        {SDL_SCANCODE_0, 7},
    {SDL_SCANCODE_1, 8},              {SDL_SCANCODE_2, 9},
    {SDL_SCANCODE_3, 10},             {SDL_SCANCODE_4, 11},
    {SDL_SCANCODE_5, 12},             {SDL_SCANCODE_6, 13},
    {SDL_SCANCODE_7, 14},             {SDL_SCANCODE_8, 15},
    {SDL_SCANCODE_9, 16},             {SDL_SCANCODE_KP_MULTIPLY, 17},
    {SDL_SCANCODE_KP_HASH, 18},
    {SDL_SCANCODE_UP, 19},            {SDL_SCANCODE_DOWN, 20},
    {SDL_SCANCODE_LEFT, 21},          {SDL_SCANCODE_RIGHT, 22},
    {SDL_SCANCODE_VOLUMEUP, 24},      {SDL_SCANCODE_VOLUMEDOWN, 25},
    {SDL_SCANCODE_POWER, 26},         {SDL_SCANCODE_CLEAR, 28},
    {SDL_SCANCODE_KP_CLEAR, 28},      {SDL_SCANCODE_CLEARAGAIN, 28},
    {SDL_SCANCODE_A, 29},             {SDL_SCANCODE_KP_A, 29},
    {SDL_SCANCODE_B, 30},             {SDL_SCANCODE_KP_B, 30},
    {SDL_SCANCODE_C, 31},             {SDL_SCANCODE_KP_C, 31},
    {SDL_SCANCODE_D, 32},             {SDL_SCANCODE_KP_D, 32},
    {SDL_SCANCODE_E, 33},             {SDL_SCANCODE_KP_E, 33},
    {SDL_SCANCODE_F, 34},             {SDL_SCANCODE_KP_F, 34},
    {SDL_SCANCODE_G, 35},
    {SDL_SCANCODE_H, 36},             {SDL_SCANCODE_I, 37},
    {SDL_SCANCODE_J, 38},             {SDL_SCANCODE_K, 39},
    {SDL_SCANCODE_L, 40},             {SDL_SCANCODE_M, 41},
    {SDL_SCANCODE_N, 42},             {SDL_SCANCODE_O, 43},
    {SDL_SCANCODE_P, 44},             {SDL_SCANCODE_Q, 45},
    {SDL_SCANCODE_R, 46},             {SDL_SCANCODE_S, 47},
    {SDL_SCANCODE_T, 48},             {SDL_SCANCODE_U, 49},
    {SDL_SCANCODE_V, 50},             {SDL_SCANCODE_W, 51},
    {SDL_SCANCODE_X, 52},             {SDL_SCANCODE_Y, 53},
    {SDL_SCANCODE_Z, 54},             {SDL_SCANCODE_COMMA, 55},
    {SDL_SCANCODE_PERIOD, 56},        {SDL_SCANCODE_KP_PERIOD, 56},
    {SDL_SCANCODE_LALT, 57},          {SDL_SCANCODE_RALT, 58},
    {SDL_SCANCODE_LSHIFT, 59},        {SDL_SCANCODE_RSHIFT, 60},
    {SDL_SCANCODE_TAB, 61},           {SDL_SCANCODE_KP_TAB, 61},
    {SDL_SCANCODE_SPACE, 62},         {SDL_SCANCODE_KP_SPACE, 62},
    {SDL_SCANCODE_MODE, 63},          {SDL_SCANCODE_WWW, 64},
    {SDL_SCANCODE_MAIL, 65},          {SDL_SCANCODE_RETURN, 66},
    {SDL_SCANCODE_RETURN2, 66},       {SDL_SCANCODE_KP_ENTER, 66},
    {SDL_SCANCODE_BACKSPACE, 67},     {SDL_SCANCODE_KP_BACKSPACE, 67},
    {SDL_SCANCODE_GRAVE, 68},
    {SDL_SCANCODE_MINUS, 69},         {SDL_SCANCODE_KP_MINUS, 69},
    {SDL_SCANCODE_EQUALS, 70},        {SDL_SCANCODE_KP_EQUALS, 70},
    {SDL_SCANCODE_KP_EQUALSAS400, 70},
    {SDL_SCANCODE_LEFTBRACKET, 71},   {SDL_SCANCODE_RIGHTBRACKET, 72},
    {SDL_SCANCODE_BACKSLASH, 73},      {SDL_SCANCODE_NONUSHASH, 73},
    {SDL_SCANCODE_NONUSBACKSLASH, 73}, {SDL_SCANCODE_SEMICOLON, 74},
    {SDL_SCANCODE_APOSTROPHE, 75},    {SDL_SCANCODE_SLASH, 76},
    {SDL_SCANCODE_KP_DIVIDE, 76},     {SDL_SCANCODE_KP_AT, 77},
    {SDL_SCANCODE_NUMLOCKCLEAR, 78},  {SDL_SCANCODE_KP_PLUS, 81},
    {SDL_SCANCODE_APPLICATION, 82},   {SDL_SCANCODE_MENU, 82},
    {SDL_SCANCODE_FIND, 84},          {SDL_SCANCODE_AC_SEARCH, 84},
    {SDL_SCANCODE_AUDIOPLAY, 85},     {SDL_SCANCODE_AUDIOSTOP, 86},
    {SDL_SCANCODE_STOP, 86},          {SDL_SCANCODE_AC_STOP, 86},
    {SDL_SCANCODE_AUDIONEXT, 87},     {SDL_SCANCODE_AUDIOPREV, 88},
    {SDL_SCANCODE_AUDIOREWIND, 89},   {SDL_SCANCODE_AUDIOFASTFORWARD, 90},
    {SDL_SCANCODE_AUDIOMUTE, 91},     {SDL_SCANCODE_MUTE, 91},
    {SDL_SCANCODE_PAGEUP, 92},        {SDL_SCANCODE_PAGEDOWN, 93},
    {SDL_SCANCODE_SELECT, 109},
    {SDL_SCANCODE_DELETE, 112},       {SDL_SCANCODE_LCTRL, 129},
    {SDL_SCANCODE_RCTRL, 130},        {SDL_SCANCODE_ESCAPE, 131},
    {SDL_SCANCODE_END, 132},          {SDL_SCANCODE_INSERT, 133},
    {SDL_SCANCODE_KP_0, 144},         {SDL_SCANCODE_KP_1, 145},
    {SDL_SCANCODE_KP_2, 146},         {SDL_SCANCODE_KP_3, 147},
    {SDL_SCANCODE_KP_4, 148},         {SDL_SCANCODE_KP_5, 149},
    {SDL_SCANCODE_KP_6, 150},         {SDL_SCANCODE_KP_7, 151},
    {SDL_SCANCODE_KP_8, 152},         {SDL_SCANCODE_KP_9, 153},
    {SDL_SCANCODE_KP_COMMA, 55},
    {SDL_SCANCODE_KP_COLON, 243},     {SDL_SCANCODE_F1, 244},
    {SDL_SCANCODE_F2, 245},           {SDL_SCANCODE_F3, 246},
    {SDL_SCANCODE_F4, 247},           {SDL_SCANCODE_F5, 248},
    {SDL_SCANCODE_F6, 249},           {SDL_SCANCODE_F7, 250},
    {SDL_SCANCODE_F8, 251},           {SDL_SCANCODE_F9, 252},
    {SDL_SCANCODE_F10, 253},          {SDL_SCANCODE_F11, 254},
    {SDL_SCANCODE_F12, 255},
});

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
    const char *legacyName = SDL_JoystickName(joystick);
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
        .legacyName = copySdlString(legacyName),
        .buttons = std::max(0, SDL_JoystickNumButtons(joystick)),
        .axes = std::max(0, SDL_JoystickNumAxes(joystick)),
        .hats = std::max(0, SDL_JoystickNumHats(joystick)),
        .playerIndex =
            controller != nullptr ? xinputPlayerIndex(pathValue) : -1};
    const int retainedButtons = std::min(
        result.buttons,
        static_cast<int>(input::kLegacyInputMaximumButtons));
    for (int button = 0; button < retainedButtons; ++button) {
      if (SDL_JoystickGetButton(joystick, button) != 0) {
        result.pressedRawButtons.push_back(button);
      }
    }
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

std::span<const SdlGdxKeyAlias> sdlGdxKeyAliases() noexcept {
  return kSdlGdxKeyAliases;
}

int gdxKeyCodeForSdlScancode(SDL_Scancode scancode) noexcept {
  const auto found = std::ranges::find(kSdlGdxKeyAliases, scancode,
                                       &SdlGdxKeyAlias::scancode);
  return found == kSdlGdxKeyAliases.end() ? -1 : found->gdxKeyCode;
}

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
  rebuildLegacyControllerGenerationLocked();
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
         .timestampMicros = toMicros(event.key.timestamp),
         .timestampDomain = input::InputTimestampDomain::SdlMilliseconds});
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
    auto found = devices_.find(event.jbutton.which);
    if (found != devices_.end()) {
      const bool pressed = event.type == SDL_JOYBUTTONDOWN;
      found->second.pressedRawButtons.set(event.jbutton.button, pressed);
      rebuildLegacyControllerGenerationLocked();
      if (!found->second.gameController) {
        publishButton(found->second, event.jbutton.button, pressed,
                      event.jbutton.timestamp);
      }
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
        .timestampMicros = toMicros(event.key.timestamp),
        .timestampDomain = input::InputTimestampDomain::SdlMilliseconds};
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
        .timestampMicros = toMicros(event.cbutton.timestamp),
        .timestampDomain = input::InputTimestampDomain::SdlMilliseconds};
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
        .timestampMicros = toMicros(event.caxis.timestamp),
        .timestampDomain = input::InputTimestampDomain::SdlMilliseconds};
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
        .timestampMicros = toMicros(event.jbutton.timestamp),
        .timestampDomain = input::InputTimestampDomain::SdlMilliseconds};
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
        .timestampMicros = toMicros(event.jaxis.timestamp),
        .timestampDomain = input::InputTimestampDomain::SdlMilliseconds};
  }
  default:
    return std::nullopt;
  }
}

std::size_t SDLInputBackend::translateRealtimeInputs(
    const SDL_Event &event, std::span<input::PhysicalInputEvent> output) {
  if (output.empty()) {
    return 0;
  }
  if (event.type != SDL_JOYHATMOTION) {
    const auto translated = translateRealtimeInput(event);
    if (!translated.has_value()) {
      return 0;
    }
    output.front() = *translated;
    return 1;
  }

  const std::lock_guard lock(devicesMutex_);
  const auto found = devices_.find(event.jhat.which);
  if (found == devices_.end() || found->second.gameController ||
      static_cast<std::size_t>(event.jhat.hat) >=
          found->second.hatValues.size()) {
    return 0;
  }
  auto &device = found->second;
  const std::size_t hat = static_cast<std::size_t>(event.jhat.hat);
  const Uint8 previous = device.hatValues[hat];
  device.hatValues[hat] = event.jhat.value;
  constexpr std::array directions{
      std::pair{SDL_HAT_UP, input::ControlDirection::Up},
      std::pair{SDL_HAT_RIGHT, input::ControlDirection::Right},
      std::pair{SDL_HAT_DOWN, input::ControlDirection::Down},
      std::pair{SDL_HAT_LEFT, input::ControlDirection::Left}};
  std::size_t count = 0;
  for (const auto &[mask, direction] : directions) {
    const bool wasPressed = (previous & mask) != 0;
    const bool pressed = (event.jhat.value & mask) != 0;
    if (wasPressed == pressed || count >= output.size()) {
      continue;
    }
    output[count++] = {
        .control = {.deviceId = device.snapshot.stableId,
                    .deviceClass = input::DeviceClass::Joystick,
                    .kind = input::ControlKind::Hat,
                    .index = event.jhat.hat,
                    .direction = direction},
        .rawValue = pressed ? 1.0 : 0.0,
        .normalizedValue = pressed ? 1.0F : 0.0F,
        .timestampMicros = toMicros(event.jhat.timestamp),
        .timestampDomain = input::InputTimestampDomain::SdlMilliseconds};
  }
  return count;
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
  std::string displayName =
      info.name.empty() ? (info.gameController ? "Game Controller" : "Joystick")
                        : info.name;
  std::string legacyName =
      info.legacyName.empty() ? displayName : std::move(info.legacyName);
  DeviceRecord record{
      .snapshot = {.stableId = std::move(stableId),
                   .displayName = std::move(displayName),
                   .deviceClass = deviceClass,
                   .connected = true,
                   .buttons = advertisedButtons,
                   .axes = advertisedAxes,
                   .hats = advertisedHats},
      .gameController = info.gameController,
      .iosAccelerometer = iosAccelerometer,
      .playerIndex = info.playerIndex,
      .legacyName = std::move(legacyName),
      .legacyOrder = nextLegacyOrder_++,
      .hatValues = std::vector<Uint8>(
          static_cast<std::size_t>(std::max(0, advertisedHats)),
          SDL_HAT_CENTERED)};
  for (const int button : info.pressedRawButtons) {
    if (button >= 0 &&
        static_cast<std::size_t>(button) <
            input::kLegacyInputMaximumButtons) {
      record.pressedRawButtons.set(static_cast<std::size_t>(button));
    }
  }
  if (publishConnection) {
    publishDevice(record.snapshot);
  }
  if (realtimeControllerMap_ && record.gameController) {
    realtimeControllerMap_->assign(record.playerIndex,
                                   record.snapshot.stableId);
  }
  devices_.emplace(info.instanceId, std::move(record));
  rebuildLegacyControllerGenerationLocked();
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
    rebuildLegacyControllerGenerationLocked();
  }
  if (disconnected.has_value()) {
    publishDevice(std::move(*disconnected));
  }
  provider_->closeDevice(instanceId);
}

void SDLInputBackend::rebuildLegacyControllerGenerationLocked() noexcept {
  std::array<const DeviceRecord *, input::kLegacyInputMaximumControllers>
      ordered{};
  std::size_t count = 0;
  for (const auto &[instanceId, device] : devices_) {
    (void)instanceId;
    std::size_t position = 0;
    while (position < count &&
           ordered[position]->legacyOrder < device.legacyOrder) {
      ++position;
    }
    if (position >= ordered.size()) {
      continue;
    }
    const std::size_t newCount = std::min(count + 1, ordered.size());
    for (std::size_t index = newCount; index > position + 1; --index) {
      ordered[index - 1] = ordered[index - 2];
    }
    ordered[position] = &device;
    count = newCount;
  }

  ++legacyControllerGeneration_.sequence;
  legacyControllerGeneration_.controllers = {};
  legacyControllerGeneration_.controllerCount = count;
  for (std::size_t index = 0; index < count; ++index) {
    legacyControllerGeneration_.controllers[index].setName(
        ordered[index]->legacyName);
    legacyControllerGeneration_.controllers[index].pressedButtons =
        ordered[index]->pressedRawButtons;
  }
}

input::LegacyInputGeneration
SDLInputBackend::legacyControllerGeneration() const noexcept {
  const std::lock_guard lock(devicesMutex_);
  return legacyControllerGeneration_;
}

void SDLInputBackend::publishButton(const DeviceRecord &device, int button,
                                    bool pressed, std::uint32_t timestamp) {
  publishInput({.control = {.deviceId = device.snapshot.stableId,
                            .deviceClass = device.snapshot.deviceClass,
                            .kind = input::ControlKind::Button,
                            .index = button},
                .rawValue = pressed ? 1.0 : 0.0,
                .normalizedValue = pressed ? 1.0F : 0.0F,
                .timestampMicros = toMicros(timestamp),
                .timestampDomain =
                    input::InputTimestampDomain::SdlMilliseconds});
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
                .timestampMicros = toMicros(timestamp),
                .timestampDomain =
                    input::InputTimestampDomain::SdlMilliseconds});
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
                  .timestampMicros = toMicros(timestamp),
                  .timestampDomain =
                      input::InputTimestampDomain::SdlMilliseconds});
  }
}
