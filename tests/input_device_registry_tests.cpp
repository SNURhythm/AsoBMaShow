#include "input/IInputBackend.h"
#include "input/InputDeviceIdentity.h"
#include "input/InputDeviceRegistry.h"
#include "input/SDLInputBackend.h"

#include <SDL2/SDL_events.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
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

  void handleSdlEvent(const SDL_Event &event) override {
    ++handledEvents;
    if (publishHandledKeyboardEvents &&
        (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)) {
      publishInput(
          {.control = {.deviceId = "keyboard",
                       .deviceClass = input::DeviceClass::Keyboard,
                       .kind = input::ControlKind::Key,
                       .index = static_cast<int>(event.key.keysym.scancode)},
           .rawValue = event.type == SDL_KEYDOWN ? 1.0 : 0.0,
           .normalizedValue = event.type == SDL_KEYDOWN ? 1.0F : 0.0F});
    }
  }

  void pump() override { ++pumpCalls; }

  void setRealtimeInputClaimed(input::DeviceClass deviceClass,
                               bool claimed) override {
    realtimeClaims.emplace_back(deviceClass, claimed);
  }

  void
  configureGyroscopeTurntable(input::GyroscopeTurntableConfig config) override {
    gyroscopeConfigs.push_back(config);
    if (publishGyroscopeReleaseOnControl) {
      publishGyroscopeRelease();
    }
  }

  void resetGyroscopeTurntableSession() override {
    ++gyroscopeResetCalls;
    if (publishGyroscopeReleaseOnControl) {
      publishGyroscopeRelease();
    }
  }

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
  int gyroscopeResetCalls = 0;
  bool publishHandledKeyboardEvents = false;
  bool publishGyroscopeReleaseOnControl = false;
  std::vector<input::GyroscopeTurntableConfig> gyroscopeConfigs;
  std::vector<std::pair<input::DeviceClass, bool>> realtimeClaims;

private:
  void publishGyroscopeRelease() {
    publishInput({.control = {.deviceId = std::string(
                                  input::kGyroscopeTurntableStableId),
                              .deviceClass = input::DeviceClass::Gyroscope,
                              .kind = input::ControlKind::Axis,
                              .index = input::kGyroscopeTurntableAxis},
                  .rawValue = 0.0,
                  .normalizedValue = 0.0F});
  }

  bool startResult_ = true;
  std::string startError_;
};

struct TrackedBackendState {
  int startCalls = 0;
  int stopCalls = 0;
  int handledEvents = 0;
  int pumpCalls = 0;
};

class FailedTrackedBackend final : public IInputBackend {
public:
  FailedTrackedBackend(input::InputBackendSink sink,
                       std::shared_ptr<TrackedBackendState> state)
      : IInputBackend(std::move(sink)), state_(std::move(state)) {}

  bool start(std::string &errorMessage) override {
    ++state_->startCalls;
    publishInput({.control = {.deviceId = "failed:backend",
                              .deviceClass = input::DeviceClass::Keyboard,
                              .kind = input::ControlKind::Key,
                              .index = 99},
                  .rawValue = 1.0,
                  .normalizedValue = 1.0F});
    errorMessage = "tracked backend unavailable";
    return false;
  }

  void stop() override { ++state_->stopCalls; }
  void handleSdlEvent(const SDL_Event &) override { ++state_->handledEvents; }
  void pump() override { ++state_->pumpCalls; }

private:
  std::shared_ptr<TrackedBackendState> state_;
};

class FakeSdlDeviceProvider final : public ISdlInputDeviceProvider {
public:
  int deviceCount() const override {
    return deviceCountOverride.value_or(static_cast<int>(devices.size()));
  }

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
    if (std::ranges::find(failingOpenIndices, deviceIndex) !=
        failingOpenIndices.end()) {
      errorMessage = "fake SDL device open failure";
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
  std::vector<int> failingOpenIndices;
  std::optional<int> deviceCountOverride;

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

  const std::string spacedPathId = identity.connect(
      {.guid = "AABB", .path = " abc ", .name = "Path With Spaces"});
  expect(spacedPathId ==
             "sdl:aabb:path:3eaf1941003943dfaa935adecffcaaa217e290def6fb0181141"
             "ced6c9daabaad",
         "path identity hashes the exact reported path without trimming");

  const std::string escapedSerialId = identity.connect(
      {.guid = "AABB", .serial = " pad:/%\x01 ", .name = "Escaped Pad"});
  expect(escapedSerialId == "sdl:aabb:serial:pad%3A%2F%25%01",
         "serial identity percent-encodes reserved and control bytes");
  const std::string utf8SerialId =
      identity.connect({.guid = "BEEF", .serial = "패드"});
  expect(utf8SerialId == "sdl:beef:serial:%ED%8C%A8%EB%93%9C",
         "serial identity percent-encodes every non-ASCII UTF-8 byte");

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

  const std::string utf8Name =
      identity.connect({.guid = "CAFE", .name = "패드"});
  expect(utf8Name == "sdl:cafe:name:%ED%8C%A8%EB%93%9C:1",
         "UTF-8 name bytes remain distinct through canonical escaping");
}

void testIdentityDisambiguatesActiveDuplicateSerials() {
  InputDeviceIdentity identity;

  const SdlDeviceIdentityDescriptor firstDescriptor{.guid = "1111",
                                                    .serial = "0",
                                                    .path = "/dev/input/twin-a",
                                                    .name = "Twin Pad"};
  const SdlDeviceIdentityDescriptor secondDescriptor{.guid = "1111",
                                                     .serial = "0",
                                                     .path =
                                                         "/dev/input/twin-b",
                                                     .name = "Twin Pad"};
  const std::string first = identity.connect(firstDescriptor);
  const std::string second = identity.connect(secondDescriptor);
  const auto remappings = identity.takeRemappings();
  const std::string effectiveFirst =
      "sdl:1111:serial:0:path:"
      "2c3fa667b7a12e8a8b88e3fa26dc3b9b6f618d4550fb62661de2398c2370637e";
  expect(first == "sdl:1111:serial:0",
         "a serialized device begins on its serial-priority ID");
  expect(second ==
             "sdl:1111:serial:0:path:"
             "0976e2d9ffd49c976c7c77e3645011e321da23d69b3be059f44bb1312d0a520e",
         "duplicate serial uses exact path evidence for disambiguation");
  expect(first != second,
         "simultaneous devices with a default serial remain independent");
  expect(remappings.size() == 1 && remappings.front().fromStableId == first &&
             remappings.front().toStableId == effectiveFirst,
         "first hotplug collision explicitly remaps the prior base owner");
  expect(identity.connect(secondDescriptor) == second,
         "overlapping instances with identical path evidence share an ID");

  identity.disconnect(effectiveFirst);
  const SdlDeviceIdentityDescriptor thirdDescriptor{.guid = "1111",
                                                    .serial = "0",
                                                    .path = "/dev/input/twin-c",
                                                    .name = "Twin Pad"};
  const std::string third = identity.connect(thirdDescriptor);
  expect(third != first && third != second,
         "historical serial path assignments reserve their effective IDs");
  expect(identity.connect(firstDescriptor) == effectiveFirst,
         "known collision owner reconnects to deterministic path ID");

  InputDeviceIdentity noPathIdentity;
  const SdlDeviceIdentityDescriptor noPath{
      .guid = "2222", .serial = "0", .name = "No Path Pad"};
  const std::string noPathFirst = noPathIdentity.connect(noPath);
  const std::string noPathSecond = noPathIdentity.connect(noPath);
  const auto noPathRemappings = noPathIdentity.takeRemappings();
  expect(noPathFirst == "sdl:2222:serial:0",
         "first pathless serialized device begins on the base ID");
  expect(noPathSecond == "sdl:2222:serial:0:ordinal:2",
         "pathless active duplicate uses a deterministic ordinal");
  expect(noPathRemappings.size() == 1 &&
             noPathRemappings.front().fromStableId == noPathFirst &&
             noPathRemappings.front().toStableId ==
                 "sdl:2222:serial:0:ordinal:1",
         "pathless collision moves the first owner to a runtime ordinal");
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

input::PhysicalInputEvent fakeKeyEvent(int index);

void testInputSubscriptionDoesNotInheritAlreadyQueuedEvents() {
  FakeBackend *backend = nullptr;
  auto registry = makeRegistryWithFakeBackend(backend);
  registry.pump();

  backend->sendInput(fakeKeyEvent(40));
  std::vector<int> received;
  registry.subscribeInput(
      [&](const auto &event) { received.push_back(event.control.index); });
  registry.pump();
  expect(received.empty(),
         "an input subscriber does not inherit pre-subscription events");

  backend->sendInput(fakeKeyEvent(41));
  registry.pump();
  expect(received == std::vector<int>({41}),
         "an input subscriber receives events queued after subscription");
}

void testSdlDispatchCompletesBeforeSceneMutationWithoutPumpingBackends() {
  FakeBackend *backend = nullptr;
  auto registry = makeRegistryWithFakeBackend(backend);
  registry.pump();
  backend->publishHandledKeyboardEvents = true;
  std::vector<std::string> order;
  registry.subscribeInput([&](const auto &event) {
    if (event.control.index == SDL_SCANCODE_S) {
      order.emplace_back("lane");
    } else if (event.control.index == SDL_SCANCODE_ESCAPE) {
      order.emplace_back("pause");
    }
  });

  SDL_Event lane{};
  lane.type = SDL_KEYDOWN;
  lane.key.keysym.scancode = SDL_SCANCODE_S;
  registry.handleSdlEventAndDispatch(lane);
  order.emplace_back("scene-after-lane");

  SDL_Event escape{};
  escape.type = SDL_KEYDOWN;
  escape.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
  registry.handleSdlEventAndDispatch(escape);
  order.emplace_back("scene-after-escape");

  expect(order == std::vector<std::string>({"lane", "scene-after-lane", "pause",
                                            "scene-after-escape"}),
         "physical input dispatch completes before each scene mutation");
  expect(backend->pumpCalls == 1,
         "per-SDL-event dispatch does not repeatedly pump native backends");
}

void testSubscriptionEpochRejectsLaterPendingEventsFromTheSamePump() {
  FakeBackend *backend = nullptr;
  auto registry = makeRegistryWithFakeBackend(backend);
  registry.pump();
  std::uint64_t lateToken = 0;
  std::vector<int> lateEvents;
  registry.subscribeInput([&](const auto &) {
    if (lateToken == 0) {
      lateToken = registry.subscribeInput([&](const auto &event) {
        lateEvents.push_back(event.control.index);
      });
    }
  });

  backend->sendInput(fakeKeyEvent(50));
  backend->sendInput(fakeKeyEvent(51));
  registry.pump();
  expect(lateEvents.empty(),
         "a new subscription rejects all events queued before its epoch");

  backend->sendInput(fakeKeyEvent(52));
  registry.pump();
  expect(lateEvents == std::vector<int>({52}),
         "a new subscription accepts the first post-subscription event");
}

void testUnsubscribedQueuedInputIsNeverReassignedToANewOwner() {
  FakeBackend *backend = nullptr;
  auto registry = makeRegistryWithFakeBackend(backend);
  registry.pump();
  int oldCallbacks = 0;
  const auto oldToken =
      registry.subscribeInput([&](const auto &) { ++oldCallbacks; });
  backend->sendInput(fakeKeyEvent(60));
  registry.unsubscribe(oldToken);
  int newCallbacks = 0;
  registry.subscribeInput([&](const auto &) { ++newCallbacks; });

  registry.pump();
  expect(oldCallbacks == 0 && newCallbacks == 0,
         "unsubscribe explicitly cancels its queued input instead of "
         "reassigning it to a new owner");
}

input::PhysicalInputEvent fakeKeyEvent(int index) {
  return {.control = {.deviceId = "keyboard",
                      .deviceClass = input::DeviceClass::Keyboard,
                      .kind = input::ControlKind::Key,
                      .index = index},
          .rawValue = 1.0,
          .normalizedValue = 1.0F};
}

void testInputSubscriptionsAreSafeDuringSamePumpMutation() {
  FakeBackend *backend = nullptr;
  auto registry = makeRegistryWithFakeBackend(backend);

  std::uint64_t firstToken = 0;
  std::uint64_t secondToken = 0;
  bool firstOwnerAlive = true;
  bool secondOwnerAlive = true;
  int callbacks = 0;
  int callbacksAfterOwnerTeardown = 0;
  firstToken = registry.subscribeInput([&](const auto &) {
    ++callbacks;
    if (!firstOwnerAlive) {
      ++callbacksAfterOwnerTeardown;
    }
    secondOwnerAlive = false;
    registry.unsubscribe(secondToken);
  });
  secondToken = registry.subscribeInput([&](const auto &) {
    ++callbacks;
    if (!secondOwnerAlive) {
      ++callbacksAfterOwnerTeardown;
    }
    firstOwnerAlive = false;
    registry.unsubscribe(firstToken);
  });
  backend->sendInput(fakeKeyEvent(10));
  registry.pump();
  expect(callbacks == 1,
         "first input callback cancels the other within the same event");
  expect(callbacksAfterOwnerTeardown == 0,
         "canceled input callback never runs after owner teardown");

  int selfCallbacks = 0;
  std::uint64_t selfToken = 0;
  selfToken = registry.subscribeInput([&](const auto &) {
    ++selfCallbacks;
    registry.unsubscribe(selfToken);
  });
  backend->sendInput(fakeKeyEvent(11));
  backend->sendInput(fakeKeyEvent(12));
  registry.pump();
  expect(selfCallbacks == 1,
         "self-unsubscribe suppresses later queued input in the same pump");

  int lateCallbacks = 0;
  std::uint64_t lateToken = 0;
  const std::uint64_t installerToken =
      registry.subscribeInput([&](const auto &) {
        if (lateToken == 0) {
          lateToken =
              registry.subscribeInput([&](const auto &) { ++lateCallbacks; });
        }
      });
  backend->sendInput(fakeKeyEvent(13));
  registry.pump();
  expect(lateCallbacks == 0,
         "listener subscribed during input dispatch skips the current event");
  backend->sendInput(fakeKeyEvent(14));
  registry.pump();
  expect(lateCallbacks == 1,
         "listener subscribed during dispatch receives later input");
  registry.unsubscribe(installerToken);
  registry.unsubscribe(lateToken);
}

void testDeviceSubscriptionsAreSafeDuringSamePumpMutation() {
  FakeBackend *backend = nullptr;
  auto registry = makeRegistryWithFakeBackend(backend);

  std::uint64_t firstToken = 0;
  std::uint64_t secondToken = 0;
  bool firstOwnerAlive = true;
  bool secondOwnerAlive = true;
  int callbacks = 0;
  int callbacksAfterOwnerTeardown = 0;
  firstToken = registry.subscribeDevices([&](const auto &) {
    ++callbacks;
    if (!firstOwnerAlive) {
      ++callbacksAfterOwnerTeardown;
    }
    secondOwnerAlive = false;
    registry.unsubscribe(secondToken);
  });
  secondToken = registry.subscribeDevices([&](const auto &) {
    ++callbacks;
    if (!secondOwnerAlive) {
      ++callbacksAfterOwnerTeardown;
    }
    firstOwnerAlive = false;
    registry.unsubscribe(firstToken);
  });
  registry.pump();
  expect(callbacks == 1,
         "first device callback cancels the other within the same event");
  expect(callbacksAfterOwnerTeardown == 0,
         "canceled device callback never runs after owner teardown");

  int selfCallbacks = 0;
  std::uint64_t selfToken = 0;
  selfToken = registry.subscribeDevices([&](const auto &) {
    ++selfCallbacks;
    registry.unsubscribe(selfToken);
  });
  backend->sendDevice({.stableId = "fake:self-first",
                       .displayName = "Self First",
                       .deviceClass = input::DeviceClass::Joystick,
                       .connected = true});
  backend->sendDevice({.stableId = "fake:self-second",
                       .displayName = "Self Second",
                       .deviceClass = input::DeviceClass::Joystick,
                       .connected = true});
  registry.pump();
  expect(selfCallbacks == 1,
         "self-unsubscribe suppresses later device events in the same pump");

  int lateCallbacks = 0;
  std::uint64_t lateToken = 0;
  const std::uint64_t installerToken =
      registry.subscribeDevices([&](const auto &) {
        if (lateToken == 0) {
          lateToken =
              registry.subscribeDevices([&](const auto &) { ++lateCallbacks; });
        }
      });
  backend->sendDevice({.stableId = "fake:first",
                       .displayName = "First",
                       .deviceClass = input::DeviceClass::Joystick,
                       .connected = true});
  registry.pump();
  expect(lateCallbacks == 0,
         "listener subscribed during device dispatch skips the current event");
  backend->sendDevice({.stableId = "fake:second",
                       .displayName = "Second",
                       .deviceClass = input::DeviceClass::Joystick,
                       .connected = true});
  registry.pump();
  expect(lateCallbacks == 1,
         "listener subscribed during dispatch receives later device events");
  registry.unsubscribe(installerToken);
  registry.unsubscribe(lateToken);
}

void testReentrantPumpPreservesQueuedEventFifo() {
  FakeBackend *backend = nullptr;
  auto registry = makeRegistryWithFakeBackend(backend);
  std::vector<int> eventOrder;
  registry.subscribeInput([&](const auto &event) {
    eventOrder.push_back(event.control.index);
    if (event.control.index == 20) {
      backend->sendInput(fakeKeyEvent(22));
      registry.pump();
    }
  });

  backend->sendInput(fakeKeyEvent(20));
  backend->sendInput(fakeKeyEvent(21));
  registry.pump();
  expect(eventOrder == std::vector<int>({20, 21, 22}),
         "nested pump appends new work after the outer pending FIFO");
}

void testRealtimeInputSubscriptionRunsBeforeRegistryPump() {
  FakeBackend *backend = nullptr;
  auto registry = makeRegistryWithFakeBackend(backend);
  std::vector<int> realtimeEvents;
  std::vector<int> frameEvents;
  const auto realtimeToken = registry.subscribeRealtimeInput(
      [&](const auto &event) { realtimeEvents.push_back(event.control.index); });
  registry.subscribeInput(
      [&](const auto &event) { frameEvents.push_back(event.control.index); });

  backend->sendInput(fakeKeyEvent(31));
  expect(realtimeEvents == std::vector<int>{31} && frameEvents.empty(),
         "realtime input subscriptions run inline without a registry pump");

  registry.pump();
  expect(frameEvents == std::vector<int>{31},
         "ordinary input subscriptions retain frame-dispatched delivery");

  registry.unsubscribe(realtimeToken);
  backend->sendInput(fakeKeyEvent(32));
  expect(realtimeEvents == std::vector<int>{31},
         "unsubscribed realtime input is revoked before unsubscribe returns");

  int selfCallbacks = 0;
  std::uint64_t selfToken = 0;
  selfToken = registry.subscribeRealtimeInput([&](const auto &) {
    ++selfCallbacks;
    registry.unsubscribe(selfToken);
  });
  backend->sendInput(fakeKeyEvent(33));
  backend->sendInput(fakeKeyEvent(34));
  expect(selfCallbacks == 1,
         "realtime input listeners can revoke themselves during delivery");

  registry.setRealtimeInputClaimed(input::DeviceClass::Keyboard, true);
  expect(backend->realtimeClaims ==
             std::vector<std::pair<input::DeviceClass, bool>>{
                 {input::DeviceClass::Keyboard, true}},
         "realtime claims activate platform input backends");
  backend->sendInput(fakeKeyEvent(35));
  registry.pump();
  expect(frameEvents == std::vector<int>({31, 32, 33, 34}),
         "claimed realtime input is omitted from ordinary frame delivery");
  registry.setRealtimeInputClaimed(input::DeviceClass::Keyboard, false);
  expect(backend->realtimeClaims.back() ==
             std::pair{input::DeviceClass::Keyboard, false},
         "realtime claim release deactivates platform input backends");
  backend->sendInput(fakeKeyEvent(36));
  registry.pump();
  expect(frameEvents == std::vector<int>({31, 32, 33, 34, 36}),
         "unclaimed input resumes ordinary delivery without a backlog");
}

void testRealtimeDeviceSubscriptionRunsBeforeRegistryPump() {
  FakeBackend *backend = nullptr;
  auto registry = makeRegistryWithFakeBackend(backend);
  std::vector<bool> realtimeConnections;
  std::vector<bool> frameConnections;
  const auto realtimeToken = registry.subscribeRealtimeDevices(
      [&](const auto &device) {
        if (device.stableId == "midi:realtime-device") {
          realtimeConnections.push_back(device.connected);
        }
      });
  registry.subscribeDevices([&](const auto &device) {
    if (device.stableId == "midi:realtime-device") {
      frameConnections.push_back(device.connected);
    }
  });

  backend->sendDevice({.stableId = "midi:realtime-device",
                       .displayName = "Realtime MIDI",
                       .deviceClass = input::DeviceClass::Midi,
                       .connected = false});
  expect(realtimeConnections == std::vector<bool>{false} &&
             frameConnections.empty(),
         "realtime device subscriptions run inline without a registry pump");
  registry.pump();
  expect(frameConnections == std::vector<bool>{false},
         "ordinary device subscriptions remain frame-dispatched");
  registry.unsubscribe(realtimeToken);
}

void testFailedBackendIsCleanedAndNeverDispatched() {
  const auto state = std::make_shared<TrackedBackendState>();
  {
    std::vector<InputDeviceRegistry::BackendFactory> factories;
    factories.emplace_back([state](input::InputBackendSink sink)
                               -> std::unique_ptr<IInputBackend> {
      return std::make_unique<FailedTrackedBackend>(std::move(sink), state);
    });
    InputDeviceRegistry registry(std::move(factories));
    expect(state->startCalls == 1, "failed backend start is attempted once");
    expect(state->stopCalls == 1,
           "failed backend receives immediate cleanup exactly once");

    SDL_Event event{};
    event.type = SDL_USEREVENT;
    registry.handleSdlEvent(event);
    int inputCallbacks = 0;
    registry.subscribeInput(
        [&](const input::PhysicalInputEvent &) { ++inputCallbacks; });
    registry.pump();
    expect(state->handledEvents == 0,
           "failed backend receives no SDL events after rejected start");
    expect(state->pumpCalls == 0,
           "failed backend receives no pump after rejected start");
    expect(inputCallbacks == 0,
           "failed backend startup publications are discarded");
    expect(containsText(registry.diagnostics(), "tracked backend unavailable"),
           "failed backend cleanup retains its startup diagnostic");
    expect(registry.isConnected("keyboard"),
           "failed backend cleanup retains the built-in keyboard");
  }
  expect(state->stopCalls == 1,
         "failed backend is not retained for destructor cleanup");
}

SdlInputDeviceInfo
controllerInfo(SDL_JoystickID instanceId,
               std::string path = "/dev/input/controller-main",
               std::string serial = {}) {
  return {.instanceId = instanceId,
          .gameController = true,
          .guid = "03000000DEAD0000BEEF000000000000",
          .serial = std::move(serial),
          .path = std::move(path),
          .name = "Arcade Controller",
          .buttons = 12,
          .axes = 6,
          .hats = 2};
}

SdlInputDeviceInfo joystickInfo(SDL_JoystickID instanceId) {
  return {.instanceId = instanceId,
          .gameController = false,
          .guid = "11110000222200003333000044440000",
          .path = "/dev/input/raw-stick",
          .name = "Raw Stick",
          .buttons = 8,
          .axes = 2,
          .hats = 1};
}

SdlInputDeviceInfo iosAccelerometerInfo(SDL_JoystickID instanceId) {
  return {.instanceId = instanceId,
          .gameController = false,
          .name = "iOS Accelerometer",
          .buttons = 0,
          .axes = 3,
          .hats = 0};
}

void testSdlEnumerationFailureKeepsKeyboardAndHotplugOperational() {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  provider->deviceCountOverride = -1;
  auto registry = makeRegistryWithSdlProvider(provider);

  std::vector<input::PhysicalInputEvent> inputEvents;
  registry.subscribeInput(
      [&](const auto &event) { inputEvents.push_back(event); });
  SDL_Event key{};
  key.type = SDL_KEYDOWN;
  key.key.keysym.scancode = SDL_SCANCODE_A;
  registry.handleSdlEvent(key);
  registry.pump();
  expect(inputEvents.size() == 1 &&
             inputEvents.front().control.deviceId == "keyboard",
         "SDL enumeration failure leaves keyboard publication operational");

  provider->deviceCountOverride.reset();
  provider->devices = {controllerInfo(88, "/dev/input/hotplug-after-fail")};
  SDL_Event added{};
  added.type = SDL_JOYDEVICEADDED;
  added.jdevice.which = 0;
  registry.handleSdlEvent(added);
  registry.pump();
  expect(std::ranges::any_of(registry.snapshot(),
                             [](const auto &device) {
                               return device.deviceClass ==
                                          input::DeviceClass::GameController &&
                                      device.connected;
                             }),
         "SDL enumeration failure leaves later hotplug operational");
}

void testSdlKeyboardFiltersRepeatAndUsesScancodes() {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  auto registry = makeRegistryWithSdlProvider(provider);
  std::vector<input::PhysicalInputEvent> inputEvents;
  registry.subscribeInput(
      [&](const auto &event) { inputEvents.push_back(event); });

  SDL_Event down{};
  down.type = SDL_KEYDOWN;
  down.key.keysym.scancode = SDL_SCANCODE_Q;
  down.key.timestamp = 7;
  registry.handleSdlEvent(down);
  SDL_Event repeat = down;
  repeat.key.repeat = 1;
  registry.handleSdlEvent(repeat);
  SDL_Event up = down;
  up.type = SDL_KEYUP;
  up.key.timestamp = 8;
  registry.handleSdlEvent(up);
  registry.pump();

  expect(inputEvents.size() == 2,
         "keyboard repeat keydown is filtered between press and release");
  expect(inputEvents.size() == 2 &&
             inputEvents[0].control.deviceId == "keyboard" &&
             inputEvents[0].control.index == SDL_SCANCODE_Q &&
             inputEvents[0].normalizedValue == 1.0F &&
             inputEvents[0].timestampMicros == 7000 &&
             inputEvents[1].normalizedValue == 0.0F &&
             inputEvents[1].timestampMicros == 8000,
         "keyboard events publish physical scancode edges and microseconds");
}

void testLegacyGenerationUsesMaintainedRawDevicesAndButtonIndices() {
  static_assert(
      std::is_trivially_copyable_v<input::LegacyInputGeneration>,
      "frame publication must remain fixed-value and allocation-free");
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  auto mapped = controllerInfo(301, "/dev/input/mapped");
  mapped.legacyName = "Mapped Raw Joystick";
  mapped.pressedRawButtons = {7};
  auto raw = joystickInfo(302);
  raw.legacyName = "Unmapped Arcade Raw";
  provider->devices = {mapped, raw};
  auto registry = makeRegistryWithSdlProvider(provider);

  SDL_Event mappedButton{};
  mappedButton.type = SDL_JOYBUTTONDOWN;
  mappedButton.jbutton.which = 301;
  mappedButton.jbutton.button = 9;
  registry.handleSdlEvent(mappedButton);
  SDL_Event rawButton = mappedButton;
  rawButton.jbutton.which = 302;
  rawButton.jbutton.button = 3;
  registry.handleSdlEvent(rawButton);
  SDL_Event key{};
  key.type = SDL_KEYDOWN;
  key.key.keysym.scancode = SDL_SCANCODE_VOLUMEUP;
  registry.handleSdlEvent(key);

  const std::size_t opensBeforeCapture = provider->openedInstances.size();
  const auto generation = registry.legacyInputGeneration(1280, 720);
  expect(generation.drawableWidth == 1280 && generation.drawableHeight == 720 &&
             generation.controllerCount == 2 &&
             generation.controllers[0].name() == "Mapped Raw Joystick" &&
             generation.controllers[0].pressedButtons.test(7) &&
             generation.controllers[0].pressedButtons.test(9) &&
             generation.controllers[1].name() == "Unmapped Arcade Raw" &&
             generation.controllers[1].pressedButtons.test(3) &&
             generation.pressedGdxKeys.test(24),
         "legacy generation preserves registry order, raw names/button indices, "
         "unmapped devices, and explicit GDX aliases");
  expect(provider->openedInstances.size() == opensBeforeCapture &&
             provider->closedInstances.empty(),
         "legacy frame capture neither enumerates nor opens/closes SDL devices");
}

void testSdlToGdxAliasTableIsExhaustiveAndUnambiguous() {
  std::array<int, SDL_NUM_SCANCODES> expected{};
  expected.fill(-1);
  bool valid = true;
  for (const auto alias : sdlGdxKeyAliases()) {
    const int scancode = static_cast<int>(alias.scancode);
    valid = valid && scancode >= 0 && scancode < SDL_NUM_SCANCODES &&
            alias.gdxKeyCode >= 0 && alias.gdxKeyCode <= 255 &&
            expected[static_cast<std::size_t>(scancode)] == -1;
    if (scancode >= 0 && scancode < SDL_NUM_SCANCODES) {
      expected[static_cast<std::size_t>(scancode)] = alias.gdxKeyCode;
    }
  }
  for (int scancode = 0; scancode < SDL_NUM_SCANCODES; ++scancode) {
    valid = valid &&
            gdxKeyCodeForSdlScancode(static_cast<SDL_Scancode>(scancode)) ==
                expected[static_cast<std::size_t>(scancode)];
  }
  expect(valid,
         "every SDL scancode has exactly its explicit representable GDX alias");
  expect(gdxKeyCodeForSdlScancode(SDL_SCANCODE_SOFTLEFT) == 1 &&
             gdxKeyCodeForSdlScancode(SDL_SCANCODE_SOFTRIGHT) == 2 &&
             gdxKeyCodeForSdlScancode(SDL_SCANCODE_ENDCALL) == 6 &&
             gdxKeyCodeForSdlScancode(SDL_SCANCODE_VOLUMEUP) == 24 &&
             gdxKeyCodeForSdlScancode(SDL_SCANCODE_VOLUMEDOWN) == 25 &&
             gdxKeyCodeForSdlScancode(SDL_SCANCODE_NUMLOCKCLEAR) == 78 &&
             gdxKeyCodeForSdlScancode(SDL_SCANCODE_WWW) == 64 &&
             gdxKeyCodeForSdlScancode(SDL_SCANCODE_MAIL) == 65 &&
             gdxKeyCodeForSdlScancode(SDL_SCANCODE_AC_SEARCH) == 84,
         "mobile, volume, Numlock, browser, mail, and search aliases match the pinned GDX codes");

  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  auto registry = makeRegistryWithSdlProvider(provider);
  SDL_Event mainEnter{};
  mainEnter.type = SDL_KEYDOWN;
  mainEnter.key.keysym.scancode = SDL_SCANCODE_RETURN;
  registry.handleSdlEvent(mainEnter);
  SDL_Event keypadEnter = mainEnter;
  keypadEnter.key.keysym.scancode = SDL_SCANCODE_KP_ENTER;
  registry.handleSdlEvent(keypadEnter);
  mainEnter.type = SDL_KEYUP;
  registry.handleSdlEvent(mainEnter);
  expect(registry.legacyInputGeneration(1, 1).pressedGdxKeys.test(66),
         "releasing one SDL alias does not clear a still-held equivalent GDX key");
  SDL_Event focusLost{};
  focusLost.type = SDL_WINDOWEVENT;
  focusLost.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
  registry.handleSdlEvent(focusLost);
  expect(!registry.legacyInputGeneration(1, 1).anyKeyPressed,
         "focus loss clears maintained keys instead of publishing stale input");
}

void testSdlInputYieldsClaimedClassesToNativeRealtimeSource() {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  auto xinput = controllerInfo(101, "XInput#0");
  xinput.playerIndex = 0;
  provider->devices = {xinput, controllerInfo(102, "hid#dualshock")};
  auto realtimeMap = std::make_shared<RealtimeControllerDeviceMap>();
  realtimeMap->setKeyboardRealtimeAvailable(true);
  std::vector<input::PhysicalInputEvent> events;
  SDLInputBackend backend(
      {.enqueueInput =
           [&](input::PhysicalInputEvent event) {
             events.push_back(std::move(event));
           },
       .enqueueDevice = [](input::InputDeviceSnapshot) {}},
      provider, realtimeMap);
  std::string error;
  expect(backend.start(error), "SDL suppression fixture starts");

  SDL_Event event{};
  event.type = SDL_KEYDOWN;
  event.key.keysym.scancode = SDL_SCANCODE_A;
  backend.setRealtimeInputClaimed(input::DeviceClass::Keyboard, true);
  backend.handleSdlEvent(event);
  expect(events.empty(),
         "claimed native keyboard input is not replayed through SDL");

  realtimeMap->setControllerRealtimeAvailable(true);
  backend.setRealtimeInputClaimed(input::DeviceClass::GameController, true);
  SDL_Event controller{};
  controller.type = SDL_CONTROLLERBUTTONDOWN;
  controller.cbutton.which = 101;
  controller.cbutton.button = SDL_CONTROLLER_BUTTON_A;
  backend.handleSdlEvent(controller);
  expect(events.empty(),
         "claimed XInput controller edges are not replayed through SDL");
  controller.cbutton.which = 102;
  backend.handleSdlEvent(controller);
  expect(events.size() == 1 &&
             events.front().control.deviceClass ==
                 input::DeviceClass::GameController,
         "non-XInput controllers retain SDL fallback delivery");

  backend.setRealtimeInputClaimed(input::DeviceClass::Keyboard, false);
  backend.handleSdlEvent(event);
  expect(events.size() == 2,
         "SDL keyboard delivery resumes after native ownership ends");
  backend.stop();
}

void testRealtimeSdlTranslationDoesNotWaitForRegistryDispatch() {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  provider->devices = {controllerInfo(91, "/dev/input/realtime-pad"),
                       joystickInfo(92)};
  auto registry = makeRegistryWithSdlProvider(provider);

  SDL_Event key{};
  key.type = SDL_KEYDOWN;
  key.key.keysym.scancode = SDL_SCANCODE_D;
  const auto translatedKey = registry.translateRealtimeSdlInput(key);

  SDL_Event button{};
  button.type = SDL_CONTROLLERBUTTONDOWN;
  button.cbutton.which = 91;
  button.cbutton.button = SDL_CONTROLLER_BUTTON_X;
  const auto translatedButton = registry.translateRealtimeSdlInput(button);

  SDL_Event joystickButton{};
  joystickButton.type = SDL_JOYBUTTONDOWN;
  joystickButton.jbutton.which = 92;
  joystickButton.jbutton.button = 3;
  const auto translatedJoystick =
      registry.translateRealtimeSdlInput(joystickButton);

  SDL_Event joystickHat{};
  joystickHat.type = SDL_JOYHATMOTION;
  joystickHat.jhat.which = 92;
  joystickHat.jhat.hat = 0;
  joystickHat.jhat.value = SDL_HAT_UP | SDL_HAT_RIGHT;
  std::array<input::PhysicalInputEvent, 4> translatedHat{};
  const std::size_t translatedHatCount =
      registry.translateRealtimeSdlInputs(joystickHat, translatedHat);
  joystickHat.jhat.value = SDL_HAT_RIGHT;
  std::array<input::PhysicalInputEvent, 4> translatedHatRelease{};
  const std::size_t translatedHatReleaseCount =
      registry.translateRealtimeSdlInputs(joystickHat, translatedHatRelease);

  SDL_Event removed{};
  removed.type = SDL_JOYDEVICEREMOVED;
  removed.jdevice.which = 91;
  const auto disconnected =
      registry.realtimeDisconnectedSdlDevice(removed);

  expect(translatedKey.has_value() &&
             translatedKey->control.deviceId == "keyboard" &&
             translatedKey->control.index == SDL_SCANCODE_D,
         "realtime SDL translation exposes a keyboard edge immediately");
  expect(translatedButton.has_value() &&
             translatedButton->control.deviceClass ==
                 input::DeviceClass::GameController &&
             translatedButton->control.kind == input::ControlKind::Button &&
             translatedButton->control.index == SDL_CONTROLLER_BUTTON_X,
         "realtime SDL translation resolves the connected controller's "
         "stable identity without pumping the registry");
  expect(translatedJoystick.has_value() &&
             translatedJoystick->control.deviceClass ==
                 input::DeviceClass::Joystick &&
             translatedJoystick->control.index == 3,
         "realtime SDL translation exposes raw joystick buttons");
  expect(translatedHatCount == 2 &&
             translatedHat[0].control.kind == input::ControlKind::Hat &&
             translatedHat[0].control.direction ==
                 input::ControlDirection::Up &&
             translatedHat[1].control.direction ==
                 input::ControlDirection::Right &&
             translatedHat[0].normalizedValue == 1.0F &&
             translatedHat[1].normalizedValue == 1.0F &&
             translatedHat[0].timestampDomain ==
                 input::InputTimestampDomain::SdlMilliseconds &&
             translatedHatReleaseCount == 1 &&
             translatedHatRelease[0].control.direction ==
                 input::ControlDirection::Up &&
             translatedHatRelease[0].normalizedValue == 0.0F,
         "realtime SDL translation expands diagonal joystick hats into "
         "ordered directional edges and preserves their releases");
  expect(disconnected.has_value() && translatedButton.has_value() &&
             *disconnected == translatedButton->control.deviceId,
         "realtime SDL removal resolves the held device before frame cleanup");
}

void testSdlRawJoystickButtonsAxesAndHatEdges() {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  provider->devices = {joystickInfo(55)};
  auto registry = makeRegistryWithSdlProvider(provider);
  registry.pump();

  std::vector<input::PhysicalInputEvent> inputEvents;
  registry.subscribeInput(
      [&](const auto &event) { inputEvents.push_back(event); });

  SDL_Event button{};
  button.type = SDL_JOYBUTTONDOWN;
  button.jbutton.which = 55;
  button.jbutton.button = 4;
  registry.handleSdlEvent(button);
  SDL_Event axis{};
  axis.type = SDL_JOYAXISMOTION;
  axis.jaxis.which = 55;
  axis.jaxis.axis = 1;
  axis.jaxis.value = 32767;
  registry.handleSdlEvent(axis);
  SDL_Event diagonal{};
  diagonal.type = SDL_JOYHATMOTION;
  diagonal.jhat.which = 55;
  diagonal.jhat.hat = 0;
  diagonal.jhat.value = SDL_HAT_UP | SDL_HAT_RIGHT;
  registry.handleSdlEvent(diagonal);
  SDL_Event centered = diagonal;
  centered.jhat.value = SDL_HAT_CENTERED;
  registry.handleSdlEvent(centered);
  registry.pump();

  expect(inputEvents.size() == 6,
         "raw joystick publishes button, axis, and four hat edges");
  expect(inputEvents.size() == 6 &&
             inputEvents[0].control.kind == input::ControlKind::Button &&
             inputEvents[0].control.index == 4 &&
             inputEvents[1].control.kind == input::ControlKind::Axis &&
             inputEvents[1].normalizedValue == 1.0F,
         "raw joystick button and positive axis endpoint are preserved");
  expect(
      inputEvents.size() == 6 &&
          inputEvents[2].control.direction == input::ControlDirection::Up &&
          inputEvents[2].normalizedValue == 1.0F &&
          inputEvents[3].control.direction == input::ControlDirection::Right &&
          inputEvents[3].normalizedValue == 1.0F &&
          inputEvents[4].control.direction == input::ControlDirection::Up &&
          inputEvents[4].normalizedValue == 0.0F &&
          inputEvents[5].control.direction == input::ControlDirection::Right &&
          inputEvents[5].normalizedValue == 0.0F,
      "diagonal hat press and centering publish directional edge pairs");
}

void testSdlIosAccelerometerMakesTiltAxesMoreSensitive() {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  provider->devices = {iosAccelerometerInfo(58)};
  auto registry = makeRegistryWithSdlProvider(provider);
  registry.pump();

  std::vector<input::PhysicalInputEvent> inputEvents;
  registry.subscribeInput(
      [&](const auto &event) { inputEvents.push_back(event); });

  for (Uint8 axisIndex = 0; axisIndex < 3; ++axisIndex) {
    SDL_Event axis{};
    axis.type = SDL_JOYAXISMOTION;
    axis.jaxis.which = 58;
    axis.jaxis.axis = axisIndex;
    axis.jaxis.value = 3277;
    registry.handleSdlEvent(axis);
  }
  registry.pump();

  const float original = 3277.0F / 32767.0F;
  expect(inputEvents.size() == 3, "iOS accelerometer publishes all axes");
  expect(inputEvents.size() == 3 &&
             std::abs(inputEvents[0].normalizedValue - original * 2.5F) <
                 0.0001F &&
             std::abs(inputEvents[1].normalizedValue - original * 2.5F) <
                 0.0001F,
         "iOS accelerometer axes 0 and 1 use 2.5x gain");
  expect(inputEvents.size() == 3 &&
             std::abs(inputEvents[2].normalizedValue - original) < 0.0001F,
         "iOS accelerometer axis 2 keeps its original sensitivity");
}

void testSdlOpenFailureIsNonFatalAndCanRecoverOnHotplug() {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  provider->devices = {joystickInfo(56)};
  provider->failingOpenIndices = {0};
  auto registry = makeRegistryWithSdlProvider(provider);
  registry.pump();
  expect(std::ranges::none_of(registry.snapshot(),
                              [](const auto &device) {
                                return device.deviceClass ==
                                       input::DeviceClass::Joystick;
                              }),
         "provider open failure publishes no phantom joystick");

  provider->failingOpenIndices.clear();
  SDL_Event added{};
  added.type = SDL_JOYDEVICEADDED;
  added.jdevice.which = 0;
  registry.handleSdlEvent(added);
  registry.pump();
  expect(std::ranges::any_of(registry.snapshot(),
                             [](const auto &device) {
                               return device.deviceClass ==
                                          input::DeviceClass::Joystick &&
                                      device.connected;
                             }),
         "a later successful hotplug recovers after open failure");
}

void testSdlBackendStartStopIsIdempotentAndClosesHandles() {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  provider->devices = {joystickInfo(57)};
  SDLInputBackend backend({.enqueueInput = [](input::PhysicalInputEvent) {},
                           .enqueueDevice = [](input::InputDeviceSnapshot) {}},
                          provider);
  std::string error;
  expect(backend.start(error), "direct SDL backend starts successfully");
  expect(backend.start(error), "repeated SDL backend start is idempotent");
  expect(provider->openedInstances == std::vector<SDL_JoystickID>({57}),
         "repeated start opens each attached handle only once");
  backend.stop();
  backend.stop();
  expect(provider->closedInstances == std::vector<SDL_JoystickID>({57}),
         "repeated stop closes each handle exactly once");

  expect(backend.start(error), "SDL backend can restart after a clean stop");
  backend.stop();
  expect(provider->openedInstances == std::vector<SDL_JoystickID>({57, 57}) &&
             provider->closedInstances == std::vector<SDL_JoystickID>({57, 57}),
         "restart owns and closes a fresh handle exactly once");
}

void testNullBackendFactoryIsDiagnosableAndHarmless() {
  std::vector<InputDeviceRegistry::BackendFactory> factories;
  factories.emplace_back(
      [](input::InputBackendSink) -> std::unique_ptr<IInputBackend> {
        return {};
      });
  InputDeviceRegistry registry(std::move(factories));
  expect(containsText(registry.diagnostics(), "factory returned null"),
         "null backend factory is retained as a diagnostic");
  expect(registry.isConnected("keyboard"),
         "null backend factory leaves keyboard observable");
  registry.pump();
}

void testRetainedBackendSinkIsClosedAfterRegistryDestruction() {
  input::InputBackendSink retainedSink;
  {
    std::vector<InputDeviceRegistry::BackendFactory> factories;
    factories.emplace_back(
        [&](input::InputBackendSink sink) -> std::unique_ptr<IInputBackend> {
          retainedSink = sink;
          return std::make_unique<FakeBackend>(std::move(sink), true,
                                               std::string{});
        });
    InputDeviceRegistry registry(std::move(factories));
    registry.pump();
  }
  expect(static_cast<bool>(retainedSink.enqueueInput) &&
             static_cast<bool>(retainedSink.enqueueDevice),
         "backend may retain closable sink functions after registry teardown");
  retainedSink.enqueueInput(fakeKeyEvent(77));
  retainedSink.enqueueDevice({.stableId = "late:device",
                              .displayName = "Late Device",
                              .deviceClass = input::DeviceClass::Joystick,
                              .connected = true});
}

struct DuplicateSerialStartupResult {
  std::string pathAId;
  std::string pathBId;
  std::vector<input::InputDeviceSnapshot> deviceEvents;
};

DuplicateSerialStartupResult
collectDuplicateSerialStartup(std::vector<SdlInputDeviceInfo> devices) {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  provider->devices = std::move(devices);
  auto registry = makeRegistryWithSdlProvider(provider);
  DuplicateSerialStartupResult result;
  registry.subscribeDevices([&](const auto &device) {
    if (device.deviceClass == input::DeviceClass::GameController) {
      result.deviceEvents.push_back(device);
    }
  });
  std::vector<input::PhysicalInputEvent> inputEvents;
  registry.subscribeInput(
      [&](const auto &event) { inputEvents.push_back(event); });
  registry.pump();

  for (const auto &device : provider->devices) {
    SDL_Event button{};
    button.type = SDL_CONTROLLERBUTTONDOWN;
    button.cbutton.which = device.instanceId;
    button.cbutton.button = device.path.ends_with("twin-a") ? 0 : 1;
    registry.handleSdlEvent(button);
  }
  registry.pump();
  for (const auto &event : inputEvents) {
    if (event.control.index == 0) {
      result.pathAId = event.control.deviceId;
    } else if (event.control.index == 1) {
      result.pathBId = event.control.deviceId;
    }
  }
  return result;
}

void testSdlStartupDuplicateSerialMappingIgnoresEnumerationOrder() {
  const auto forward = collectDuplicateSerialStartup(
      {controllerInfo(30, "/dev/input/twin-a", "0"),
       controllerInfo(31, "/dev/input/twin-b", "0")});
  const auto reverse = collectDuplicateSerialStartup(
      {controllerInfo(41, "/dev/input/twin-b", "0"),
       controllerInfo(40, "/dev/input/twin-a", "0")});

  const std::string base = "sdl:03000000dead0000beef000000000000:serial:0";
  const std::string expectedA =
      base + ":path:"
             "2c3fa667b7a12e8a8b88e3fa26dc3b9b6f618d4550fb62661de2398c2370637e";
  const std::string expectedB =
      base + ":path:"
             "0976e2d9ffd49c976c7c77e3645011e321da23d69b3be059f44bb1312d0a520e";
  expect(forward.pathAId == expectedA && reverse.pathAId == expectedA,
         "path A keeps one effective ID across fresh reverse enumeration");
  expect(forward.pathBId == expectedB && reverse.pathBId == expectedB,
         "path B keeps one effective ID across fresh reverse enumeration");
  expect(forward.deviceEvents.size() == 2 && reverse.deviceEvents.size() == 2 &&
             std::ranges::none_of(
                 forward.deviceEvents,
                 [&](const auto &event) { return event.stableId == base; }) &&
             std::ranges::none_of(
                 reverse.deviceEvents,
                 [&](const auto &event) { return event.stableId == base; }),
         "staged startup publishes only final collision IDs, never a base ID");
}

void testSdlSoleSerializedDevicePathChurnKeepsBaseId() {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  provider->devices = {
      controllerInfo(50, "/dev/input/serial-old-port", "SERIAL-CHURN")};
  auto registry = makeRegistryWithSdlProvider(provider);
  std::vector<input::InputDeviceSnapshot> deviceEvents;
  registry.subscribeDevices([&](const auto &device) {
    if (device.deviceClass == input::DeviceClass::GameController) {
      deviceEvents.push_back(device);
    }
  });
  registry.pump();
  const std::string base =
      "sdl:03000000dead0000beef000000000000:serial:SERIAL-CHURN";
  expect(deviceEvents.size() == 1 && deviceEvents.front().stableId == base,
         "sole serialized device starts on its serial-priority base ID");

  SDL_Event removed{};
  removed.type = SDL_JOYDEVICEREMOVED;
  removed.jdevice.which = 50;
  registry.handleSdlEvent(removed);
  registry.pump();
  deviceEvents.clear();

  provider->devices[0] =
      controllerInfo(51, "/dev/input/serial-new-port", "SERIAL-CHURN");
  SDL_Event added{};
  added.type = SDL_JOYDEVICEADDED;
  added.jdevice.which = 0;
  registry.handleSdlEvent(added);
  registry.pump();
  expect(deviceEvents.size() == 1 && deviceEvents.front().connected &&
             deviceEvents.front().stableId == base,
         "sole serialized reconnect reuses base ID after exact path changes");
  expect(registry.isConnected(base),
         "path churn leaves the serial-priority binding target connected");
}

void testSdlHotplugCollisionRemapsAndRetainsDeterministicLedger() {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  provider->devices = {controllerInfo(60, "/dev/input/twin-a", "0")};
  auto registry = makeRegistryWithSdlProvider(provider);
  std::vector<input::InputDeviceSnapshot> deviceEvents;
  registry.subscribeDevices([&](const auto &device) {
    if (device.deviceClass == input::DeviceClass::GameController) {
      deviceEvents.push_back(device);
    }
  });
  registry.pump();
  const std::string base = "sdl:03000000dead0000beef000000000000:serial:0";
  const std::string pathAId =
      base + ":path:"
             "2c3fa667b7a12e8a8b88e3fa26dc3b9b6f618d4550fb62661de2398c2370637e";
  const std::string pathBId =
      base + ":path:"
             "0976e2d9ffd49c976c7c77e3645011e321da23d69b3be059f44bb1312d0a520e";
  expect(deviceEvents.size() == 1 && deviceEvents.front().stableId == base,
         "unique serialized hotplug baseline starts on base ID");
  deviceEvents.clear();

  provider->devices.push_back(controllerInfo(61, "/dev/input/twin-b", "0"));
  SDL_Event added{};
  added.type = SDL_JOYDEVICEADDED;
  added.jdevice.which = 1;
  registry.handleSdlEvent(added);
  registry.pump();
  expect(
      deviceEvents.size() == 3 && deviceEvents[0].stableId == base &&
          !deviceEvents[0].connected && deviceEvents[1].stableId == pathAId &&
          deviceEvents[1].connected && deviceEvents[2].stableId == pathBId &&
          deviceEvents[2].connected,
      "first hotplug collision atomically retires base then publishes paths");

  std::vector<input::PhysicalInputEvent> inputEvents;
  registry.subscribeInput(
      [&](const auto &event) { inputEvents.push_back(event); });
  for (const auto [instanceId, buttonIndex] :
       {std::pair<SDL_JoystickID, Uint8>{60, 0},
        std::pair<SDL_JoystickID, Uint8>{61, 1}}) {
    SDL_Event button{};
    button.type = SDL_CONTROLLERBUTTONDOWN;
    button.cbutton.which = instanceId;
    button.cbutton.button = buttonIndex;
    registry.handleSdlEvent(button);
  }
  registry.pump();
  expect(inputEvents.size() == 2 &&
             inputEvents[0].control.deviceId == pathAId &&
             inputEvents[1].control.deviceId == pathBId,
         "hotplug collision remaps every live input record to final path ID");

  deviceEvents.clear();
  SDL_Event removed{};
  removed.type = SDL_JOYDEVICEREMOVED;
  removed.jdevice.which = 61;
  registry.handleSdlEvent(removed);
  registry.pump();
  expect(deviceEvents.size() == 1 && deviceEvents.front().stableId == pathBId &&
             !deviceEvents.front().connected && registry.isConnected(pathAId),
         "duplicate removal leaves the other deterministic path owner live");

  provider->devices.push_back(controllerInfo(62, "/dev/input/twin-b", "0"));
  added.jdevice.which = 2;
  registry.handleSdlEvent(added);
  registry.pump();
  expect(deviceEvents.size() == 2 && deviceEvents.back().stableId == pathBId &&
             deviceEvents.back().connected,
         "duplicate re-add after a gap reuses its deterministic path ID");

  deviceEvents.clear();
  removed.jdevice.which = 60;
  registry.handleSdlEvent(removed);
  removed.jdevice.which = 62;
  registry.handleSdlEvent(removed);
  registry.pump();
  provider->devices.push_back(controllerInfo(63, "/dev/input/twin-a", "0"));
  added.jdevice.which = 3;
  registry.handleSdlEvent(added);
  registry.pump();
  expect(deviceEvents.size() == 3 && deviceEvents.back().stableId == pathAId &&
             deviceEvents.back().connected,
         "known collision ledger prevents base reuse after all owners gap");
}

void testSdlDuplicateSerialsKeepIndependentEffectiveIds() {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  provider->devices = {
      controllerInfo(30, "/dev/input/twin-a", "0"),
      controllerInfo(31, "/dev/input/twin-b", "0"),
  };
  auto registry = makeRegistryWithSdlProvider(provider);
  registry.pump();

  const auto devices = registry.snapshot();
  std::vector<std::string> controllerIds;
  for (const auto &device : devices) {
    if (device.deviceClass == input::DeviceClass::GameController) {
      controllerIds.push_back(device.stableId);
    }
  }
  expect(controllerIds.size() == 2,
         "both controllers with a duplicated serial remain observable");
  expect(controllerIds.size() == 2 && controllerIds[0] != controllerIds[1],
         "duplicated serial controllers receive independent effective IDs");

  std::vector<input::PhysicalInputEvent> inputEvents;
  registry.subscribeInput(
      [&](const auto &event) { inputEvents.push_back(event); });
  for (const SDL_JoystickID instanceId : {30, 31}) {
    SDL_Event button{};
    button.type = SDL_CONTROLLERBUTTONDOWN;
    button.cbutton.which = instanceId;
    button.cbutton.button = 2;
    registry.handleSdlEvent(button);
  }
  registry.pump();
  std::vector<std::string> inputIds;
  for (const auto &event : inputEvents) {
    inputIds.push_back(event.control.deviceId);
  }
  std::ranges::sort(inputIds);
  std::ranges::sort(controllerIds);
  expect(inputIds == controllerIds,
         "input records use each controller's effective stable ID");

  SDL_Event removed{};
  removed.type = SDL_JOYDEVICEREMOVED;
  removed.jdevice.which = 30;
  registry.handleSdlEvent(removed);
  registry.pump();
  const std::string serialBase =
      "sdl:03000000dead0000beef000000000000:serial:0";
  const std::string pathAId =
      serialBase +
      ":path:"
      "2c3fa667b7a12e8a8b88e3fa26dc3b9b6f618d4550fb62661de2398c2370637e";
  const std::string pathBId =
      serialBase +
      ":path:"
      "0976e2d9ffd49c976c7c77e3645011e321da23d69b3be059f44bb1312d0a520e";
  expect(!registry.isConnected(pathAId),
         "removed duplicate serial owner becomes disconnected independently");
  expect(registry.isConnected(pathBId),
         "removing one duplicated serial owner leaves the other connected");
}

void testSdlOverlappingReconnectWaitsForLastOwnerRemoval() {
  auto provider = std::make_shared<FakeSdlDeviceProvider>();
  provider->devices = {controllerInfo(42)};
  auto registry = makeRegistryWithSdlProvider(provider);

  std::vector<input::InputDeviceSnapshot> deviceEvents;
  registry.subscribeDevices([&](const auto &device) {
    if (device.deviceClass == input::DeviceClass::GameController) {
      deviceEvents.push_back(device);
    }
  });
  registry.pump();
  expect(deviceEvents.size() == 1 && deviceEvents.front().connected,
         "overlap test begins with one connected controller");
  if (deviceEvents.empty()) {
    return;
  }
  const std::string stableId = deviceEvents.front().stableId;
  deviceEvents.clear();

  provider->devices.push_back(controllerInfo(77));
  SDL_Event added{};
  added.type = SDL_JOYDEVICEADDED;
  added.jdevice.which = 1;
  registry.handleSdlEvent(added);
  registry.pump();
  expect(deviceEvents.empty(),
         "overlapping instance does not republish an already-live device");

  std::vector<input::PhysicalInputEvent> inputEvents;
  registry.subscribeInput(
      [&](const auto &event) { inputEvents.push_back(event); });
  SDL_Event button{};
  button.type = SDL_CONTROLLERBUTTONDOWN;
  button.cbutton.which = 77;
  button.cbutton.button = 1;
  registry.handleSdlEvent(button);
  registry.pump();
  expect(inputEvents.size() == 1 &&
             inputEvents.front().control.deviceId == stableId,
         "overlapping instance input uses the shared effective stable ID");

  SDL_Event removed{};
  removed.type = SDL_JOYDEVICEREMOVED;
  removed.jdevice.which = 42;
  registry.handleSdlEvent(removed);
  registry.pump();
  expect(deviceEvents.empty(),
         "removing an old overlapping instance emits no disconnect");
  expect(registry.isConnected(stableId),
         "stable device remains connected while a new owner is live");

  removed.jdevice.which = 77;
  registry.handleSdlEvent(removed);
  registry.pump();
  expect(deviceEvents.size() == 1 && !deviceEvents.front().connected,
         "last live owner removal publishes connected=false exactly once");
  expect(!registry.isConnected(stableId),
         "stable device disconnects after its final owner is removed");
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
  expect(deviceEvents.front().buttons == SDL_CONTROLLER_BUTTON_MAX &&
             deviceEvents.front().axes == SDL_CONTROLLER_AXIS_MAX &&
             deviceEvents.front().hats == 0,
         "controller snapshot advertises only standardized controller inputs");

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
  SDL_Event suppressedRawHat{};
  suppressedRawHat.type = SDL_JOYHATMOTION;
  suppressedRawHat.jhat.which = 42;
  suppressedRawHat.jhat.hat = 0;
  suppressedRawHat.jhat.value = SDL_HAT_UP;
  registry.handleSdlEvent(suppressedRawHat);
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

void testGyroscopeControlFanoutDispatchesWithoutBackendPump() {
  FakeBackend *first = nullptr;
  FakeBackend *second = nullptr;
  std::vector<InputDeviceRegistry::BackendFactory> factories;
  factories.emplace_back(
      [&](input::InputBackendSink sink) -> std::unique_ptr<IInputBackend> {
        auto backend =
            std::make_unique<FakeBackend>(std::move(sink), true, std::string{});
        first = backend.get();
        return backend;
      });
  factories.emplace_back(
      [&](input::InputBackendSink sink) -> std::unique_ptr<IInputBackend> {
        auto backend =
            std::make_unique<FakeBackend>(std::move(sink), true, std::string{});
        second = backend.get();
        return backend;
      });
  InputDeviceRegistry registry(std::move(factories));
  first->publishGyroscopeReleaseOnControl = true;
  second->publishGyroscopeReleaseOnControl = true;
  std::vector<input::PhysicalInputEvent> events;
  registry.subscribeInput([&](const auto &event) { events.push_back(event); });

  const input::GyroscopeTurntableConfig config{.stepAngleDegrees = 7,
                                               .releaseDelayMs = 350};
  registry.configureGyroscopeTurntable(config);
  expect(first->gyroscopeConfigs == std::vector{config} &&
             second->gyroscopeConfigs == std::vector{config},
         "gyroscope configuration fans out to every backend");
  expect(events.size() == 2 && first->pumpCalls == 0 && second->pumpCalls == 0,
         "configuration-generated zero dispatches synchronously without pump");

  registry.resetGyroscopeTurntableSession();
  expect(first->gyroscopeResetCalls == 1 && second->gyroscopeResetCalls == 1,
         "gyroscope session reset fans out to every backend");
  expect(events.size() == 4 && first->pumpCalls == 0 && second->pumpCalls == 0,
         "reset-generated zero dispatches synchronously without async pump");
  expect(events.back().control.deviceId == input::kGyroscopeTurntableStableId &&
             events.back().normalizedValue == 0.0F,
         "synchronous release preserves gyroscope semantic identity");
}
} // namespace

int main() {
  testIdentityPrecedenceAndNameOrdinals();
  testIdentityDisambiguatesActiveDuplicateSerials();
  testRegistryQueuesCallbacksAndKeepsKeyboard();
  testInputSubscriptionDoesNotInheritAlreadyQueuedEvents();
  testSdlDispatchCompletesBeforeSceneMutationWithoutPumpingBackends();
  testSubscriptionEpochRejectsLaterPendingEventsFromTheSamePump();
  testUnsubscribedQueuedInputIsNeverReassignedToANewOwner();
  testInputSubscriptionsAreSafeDuringSamePumpMutation();
  testDeviceSubscriptionsAreSafeDuringSamePumpMutation();
  testReentrantPumpPreservesQueuedEventFifo();
  testRealtimeInputSubscriptionRunsBeforeRegistryPump();
  testRealtimeDeviceSubscriptionRunsBeforeRegistryPump();
  testFailedBackendIsCleanedAndNeverDispatched();
  testSdlEnumerationFailureKeepsKeyboardAndHotplugOperational();
  testSdlKeyboardFiltersRepeatAndUsesScancodes();
  testLegacyGenerationUsesMaintainedRawDevicesAndButtonIndices();
  testSdlToGdxAliasTableIsExhaustiveAndUnambiguous();
  testSdlInputYieldsClaimedClassesToNativeRealtimeSource();
  testRealtimeSdlTranslationDoesNotWaitForRegistryDispatch();
  testSdlRawJoystickButtonsAxesAndHatEdges();
  testSdlIosAccelerometerMakesTiltAxesMoreSensitive();
  testSdlOpenFailureIsNonFatalAndCanRecoverOnHotplug();
  testSdlBackendStartStopIsIdempotentAndClosesHandles();
  testNullBackendFactoryIsDiagnosableAndHarmless();
  testRetainedBackendSinkIsClosedAfterRegistryDestruction();
  testSdlStartupDuplicateSerialMappingIgnoresEnumerationOrder();
  testSdlSoleSerializedDevicePathChurnKeepsBaseId();
  testSdlHotplugCollisionRemapsAndRetainsDeterministicLedger();
  testSdlDuplicateSerialsKeepIndependentEffectiveIds();
  testSdlOverlappingReconnectWaitsForLastOwnerRemoval();
  testSdlReconnectRemovalDedupAndAxisNormalization();
  testSdlIdenticalNameOnlyDevicesUseDistinctOrdinals();
  testGyroscopeControlFanoutDispatchesWithoutBackendPump();

  if (failures != 0) {
    std::cerr << failures << " input device registry assertion(s) failed\n";
    return 1;
  }
  std::cout << "input device registry tests passed\n";
  return 0;
}
