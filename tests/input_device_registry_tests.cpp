#include "input/IInputBackend.h"
#include "input/InputDeviceIdentity.h"
#include "input/InputDeviceRegistry.h"
#include "input/SDLInputBackend.h"

#include <SDL2/SDL_events.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

bool containsText(const std::vector<std::string> &values,
                  std::string_view needle) {
  return std::ranges::any_of(values, [&](const std::string &value) {
    return value.find(needle) != std::string::npos;
  });
}

const input::InputDeviceSnapshot *
findDevice(const std::vector<input::InputDeviceSnapshot> &devices,
           std::string_view stableId) {
  const auto found = std::ranges::find_if(
      devices, [&](const auto &device) { return device.stableId == stableId; });
  return found == devices.end() ? nullptr : &*found;
}

class FakeBackend final : public IInputBackend {
public:
  FakeBackend(input::InputBackendSink sink, bool startResult,
              std::string startError)
      : IInputBackend(std::move(sink)), startResult_(startResult),
        startError_(std::move(startError)) {}

  bool start(std::string &errorMessage) override {
    ++startCalls;
    if (!startResult_) {
      errorMessage = startError_;
    }
    return startResult_;
  }

  void stop() override { ++stopCalls; }

  void handleSdlEvent(const SDL_Event &) override { ++handledEvents; }

  void pump() override { ++pumpCalls; }

  void sendInput(input::PhysicalInputEvent event) {
    publishInput(std::move(event));
  }

  void sendDevice(input::InputDeviceSnapshot device) {
    publishDevice(std::move(device));
  }

  int startCalls = 0;
  int stopCalls = 0;
  int handledEvents = 0;
  int pumpCalls = 0;

private:
  bool startResult_ = true;
  std::string startError_;
};

class FakeSdlDeviceProvider final : public ISdlInputDeviceProvider {
public:
  int deviceCount() const override { return static_cast<int>(devices.size()); }

  bool isGameController(int deviceIndex) const override {
    return validIndex(deviceIndex) && devices[deviceIndex].gameController;
  }

  std::optional<SdlInputDeviceInfo>
  openDevice(int deviceIndex, bool asGameController,
             std::string &errorMessage) override {
    if (!validIndex(deviceIndex)) {
      errorMessage = "fake SDL device index is out of range";
      return std::nullopt;
    }
    SdlInputDeviceInfo result = devices[deviceIndex];
    if (result.gameController != asGameController) {
      errorMessage = "fake SDL device class mismatch";
      return std::nullopt;
    }
    openedInstances.push_back(result.instanceId);
    return result;
  }

  void closeDevice(SDL_JoystickID instanceId) override {
    closedInstances.push_back(instanceId);
  }

  std::vector<SdlInputDeviceInfo> devices;
  std::vector<SDL_JoystickID> openedInstances;
  std::vector<SDL_JoystickID> closedInstances;

private:
  bool validIndex(int deviceIndex) const {
    return deviceIndex >= 0 &&
           static_cast<std::size_t>(deviceIndex) < devices.size();
  }
};

InputDeviceRegistry makeRegistryWithFakeBackend(FakeBackend *&backend,
                                                bool startResult = true,
                                                std::string error = {}) {
  std::vector<InputDeviceRegistry::BackendFactory> factories;
  factories.emplace_back(
      [&](input::InputBackendSink sink) -> std::unique_ptr<IInputBackend> {
        auto result = std::make_unique<FakeBackend>(
            std::move(sink), startResult, std::move(error));
        backend = result.get();
        return result;
      });
  return InputDeviceRegistry(std::move(factories));
}

InputDeviceRegistry makeRegistryWithSdlProvider(
    const std::shared_ptr<ISdlInputDeviceProvider> &provider) {
  std::vector<InputDeviceRegistry::BackendFactory> factories;
  factories.emplace_back([provider](input::InputBackendSink sink)
                             -> std::unique_ptr<IInputBackend> {
    return std::make_unique<SDLInputBackend>(std::move(sink), provider);
  });
  return InputDeviceRegistry(std::move(factories));
}

void testIdentityPrecedenceAndNameOrdinals() {
  InputDeviceIdentity identity;

  const std::string serialId = identity.connect(
      {.guid = "AABB", .serial = " Serial-7 ", .path = "abc", .name = "Pad"});
  expect(serialId == "sdl:aabb:serial:Serial-7",
         "serial identity has first priority");

  const std::string pathId =
      identity.connect({.guid = "AABB", .path = "abc", .name = "Pad"});
  expect(pathId ==
             "sdl:aabb:path:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb"
             "410ff61f20015ad",
         "path identity uses an exact SHA-256 digest");

  const SdlDeviceIdentityDescriptor unnamed{.guid = "AABB",
                                            .name = "  Twin PAD / Pro  "};
  const std::string first = identity.connect(unnamed);
  const std::string second = identity.connect(unnamed);
  expect(first == "sdl:aabb:name:twin-pad-pro:1",
         "first serial-less duplicate receives ordinal one");
  expect(second == "sdl:aabb:name:twin-pad-pro:2",
         "identical serial-less devices receive distinct ordinals");
  identity.disconnect(first);
  expect(identity.connect(unnamed) == first,
         "a reconnect reuses the disconnected stable ordinal");
}

void testRegistryQueuesCallbacksAndKeepsKeyboard() {
  FakeBackend *backend = nullptr;
  auto registry = makeRegistryWithFakeBackend(backend);
  expect(backend != nullptr && backend->startCalls == 1,
         "registry starts injected backends once");

  const auto initial = registry.snapshot();
  const auto *keyboard = findDevice(initial, "keyboard");
  expect(keyboard != nullptr,
         "keyboard is always present with stable ID keyboard");
  expect(keyboard != nullptr && keyboard->connected,
         "keyboard is always connected");
  expect(keyboard != nullptr &&
             keyboard->deviceClass == input::DeviceClass::Keyboard,
         "keyboard snapshot has keyboard class");

  int inputCallbacks = 0;
  const std::uint64_t inputToken = registry.subscribeInput(
      [&](const input::PhysicalInputEvent &) { ++inputCallbacks; });
  backend->sendInput({.control = {.deviceId = "keyboard",
                                  .deviceClass = input::DeviceClass::Keyboard,
                                  .kind = input::ControlKind::Key,
                                  .index = 4},
                      .rawValue = 1.0,
                      .normalizedValue = 1.0f,
                      .timestampMicros = 10});
  expect(inputCallbacks == 0, "backend input never calls listeners inline");
  registry.pump();
  expect(inputCallbacks == 1, "input listeners execute during registry pump");
  expect(backend->pumpCalls == 1, "registry pumps each backend once");

  registry.unsubscribe(inputToken);
  backend->sendInput({.control = {.deviceId = "keyboard",
                                  .deviceClass = input::DeviceClass::Keyboard,
                                  .kind = input::ControlKind::Key,
                                  .index = 5},
                      .rawValue = 1.0,
                      .normalizedValue = 1.0f});
  registry.pump();
  expect(inputCallbacks == 1, "unsubscribe prevents later input callbacks");

  int deviceCallbacks = 0;
  registry.subscribeDevices(
      [&](const input::InputDeviceSnapshot &) { ++deviceCallbacks; });
  backend->sendDevice({.stableId = "fake:pad",
                       .displayName = "Fake Pad",
                       .deviceClass = input::DeviceClass::Joystick,
                       .connected = false});
  expect(deviceCallbacks == 0, "device listeners are also pump-only");
  registry.pump();
  expect(deviceCallbacks == 1, "device listener runs during pump");
  expect(!registry.isConnected("fake:pad"),
         "disconnected snapshots remain queryable as disconnected");

  SDL_Event event{};
  event.type = SDL_USEREVENT;
  registry.handleSdlEvent(event);
  expect(backend->handledEvents == 1,
         "registry forwards every SDL event to each backend");
}

void testBackendStartFailureIsDiagnosableAndKeyboardSurvives() {
  FakeBackend *backend = nullptr;
  auto registry =
      makeRegistryWithFakeBackend(backend, false, "fake backend unavailable");
  expect(backend != nullptr && backend->startCalls == 1,
         "failing backend start is attempted once");
  expect(containsText(registry.diagnostics(), "fake backend unavailable"),
         "backend start failure remains diagnosable");
  expect(registry.isConnected("keyboard"),
         "backend start failure never disables keyboard");
}

SdlInputDeviceInfo controllerInfo(SDL_JoystickID instanceId) {
  return {.instanceId = instanceId,
          .gameController = true,
          .guid = "03000000DEAD0000BEEF000000000000",
          .serial = "",
          .path = "/dev/input/controller-main",
          .name = "Arcade Controller",
          .buttons = 12,
          .axes = 6,
          .hats = 0};
}

void testSdlReconnectRemovalDedupAndAxisNormalization() {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  provider->devices = {controllerInfo(42)};
  auto registry = makeRegistryWithSdlProvider(provider);

  std::vector<input::InputDeviceSnapshot> deviceEvents;
  registry.subscribeDevices([&](const auto &device) {
    if (device.deviceClass != input::DeviceClass::Keyboard) {
      deviceEvents.push_back(device);
    }
  });
  registry.pump();
  expect(deviceEvents.size() == 1 && deviceEvents.front().connected,
         "SDL controller connection is published during pump");
  if (deviceEvents.empty()) {
    return;
  }
  const std::string stableId = deviceEvents.front().stableId;
  expect(stableId.find("42") == std::string::npos,
         "volatile SDL instance ID is absent from stable persistence ID");

  std::vector<input::PhysicalInputEvent> inputEvents;
  registry.subscribeInput(
      [&](const auto &event) { inputEvents.push_back(event); });

  SDL_Event controllerButton{};
  controllerButton.type = SDL_CONTROLLERBUTTONDOWN;
  controllerButton.cbutton.which = 42;
  controllerButton.cbutton.button = 3;
  controllerButton.cbutton.state = SDL_PRESSED;
  controllerButton.cbutton.timestamp = 9;
  registry.handleSdlEvent(controllerButton);

  SDL_Event duplicateRawButton{};
  duplicateRawButton.type = SDL_JOYBUTTONDOWN;
  duplicateRawButton.jbutton.which = 42;
  duplicateRawButton.jbutton.button = 3;
  duplicateRawButton.jbutton.state = SDL_PRESSED;
  duplicateRawButton.jbutton.timestamp = 9;
  registry.handleSdlEvent(duplicateRawButton);
  expect(inputEvents.empty(), "SDL events enqueue without inline listeners");
  registry.pump();
  expect(inputEvents.size() == 1,
         "controller input is not duplicated as a raw joystick event");
  expect(inputEvents.size() == 1 && inputEvents.front().control.deviceClass ==
                                        input::DeviceClass::GameController,
         "controller event retains controller device class");

  inputEvents.clear();
  SDL_Event axis{};
  axis.type = SDL_CONTROLLERAXISMOTION;
  axis.caxis.which = 42;
  axis.caxis.axis = 1;
  axis.caxis.value = -32768;
  registry.handleSdlEvent(axis);
  registry.pump();
  expect(inputEvents.size() == 1 &&
             inputEvents.front().normalizedValue == -1.0f,
         "SDL axis minimum normalizes exactly to -1.0f");

  deviceEvents.clear();
  SDL_Event removed{};
  removed.type = SDL_JOYDEVICEREMOVED;
  removed.jdevice.which = 42;
  registry.handleSdlEvent(removed);
  expect(deviceEvents.empty(), "removal remains queued before pump");
  registry.pump();
  expect(deviceEvents.size() == 1 && !deviceEvents.front().connected,
         "removal publishes connected=false");
  expect(!registry.isConnected(stableId),
         "removed stable device is retained as disconnected");

  provider->devices[0] = controllerInfo(77);
  SDL_Event added{};
  added.type = SDL_JOYDEVICEADDED;
  added.jdevice.which = 0;
  registry.handleSdlEvent(added);
  registry.pump();
  expect(deviceEvents.size() == 2 && deviceEvents.back().connected,
         "reconnected controller publishes connected=true");
  expect(deviceEvents.size() == 2 && deviceEvents.back().stableId == stableId,
         "reconnecting with a new SDL instance keeps its stable ID");
}

void testSdlIdenticalNameOnlyDevicesUseDistinctOrdinals() {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  provider->devices = {
      {.instanceId = 10,
       .gameController = false,
       .guid = "1111",
       .name = "Twin Stick",
       .buttons = 8,
       .axes = 2,
       .hats = 1},
      {.instanceId = 11,
       .gameController = false,
       .guid = "1111",
       .name = "Twin Stick",
       .buttons = 8,
       .axes = 2,
       .hats = 1},
  };
  auto registry = makeRegistryWithSdlProvider(provider);
  registry.pump();

  auto devices = registry.snapshot();
  std::vector<std::string> joystickIds;
  for (const auto &device : devices) {
    if (device.deviceClass == input::DeviceClass::Joystick) {
      joystickIds.push_back(device.stableId);
    }
  }
  std::ranges::sort(joystickIds);
  expect(joystickIds.size() == 2, "both identical joysticks are registered");
  expect(joystickIds.size() == 2 && joystickIds[0] != joystickIds[1],
         "identical serial-less joysticks have distinct stable IDs");
  expect(joystickIds.size() == 2 && joystickIds[0].ends_with(":1") &&
             joystickIds[1].ends_with(":2"),
         "serial-less stable IDs use deterministic ordinals");
}
} // namespace

int main() {
  testIdentityPrecedenceAndNameOrdinals();
  testRegistryQueuesCallbacksAndKeepsKeyboard();
  testBackendStartFailureIsDiagnosableAndKeyboardSurvives();
  testSdlReconnectRemovalDedupAndAxisNormalization();
  testSdlIdenticalNameOnlyDevicesUseDistinctOrdinals();

  if (failures != 0) {
    std::cerr << failures << " input device registry assertion(s) failed\n";
    return 1;
  }
  std::cout << "input device registry tests passed\n";
  return 0;
}
