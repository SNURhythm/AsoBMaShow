// Compile the real capture controller/resolver with a failing registry boundary.
#include "input/InputCaptureController.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
enum class Failure { None, Input, Device };
Failure failure = Failure::None;
std::map<std::uint64_t, InputDeviceRegistry::InputListener> inputs;
std::map<std::uint64_t, InputDeviceRegistry::DeviceListener> devices;
std::uint64_t nextToken = 1;
void require(bool value, const char *message) {
  if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
} // namespace

InputDeviceRegistry::InputDeviceRegistry() = default;
InputDeviceRegistry::~InputDeviceRegistry() = default;
std::uint64_t InputDeviceRegistry::subscribeInput(InputListener listener) {
  if (failure == Failure::Input) throw std::runtime_error("injected registry failure");
  const auto token = nextToken++;
  inputs.emplace(token, std::move(listener));
  return token;
}
std::uint64_t InputDeviceRegistry::subscribeDevices(DeviceListener listener) {
  if (failure == Failure::Device) throw std::runtime_error("injected registry failure");
  const auto token = nextToken++;
  devices.emplace(token, std::move(listener));
  return token;
}
void InputDeviceRegistry::unsubscribe(std::uint64_t token) {
  require(inputs.erase(token) + devices.erase(token) == 1,
          "controller must unsubscribe each acquired token exactly once");
}
bool InputDeviceRegistry::isConnected(std::string_view) const { return true; }

void checkConstruction(Failure injectedFailure) {
  InputDeviceRegistry registry;
  InputProfile profile;
  int unrelatedCalls = 0;
  const auto unrelated = registry.subscribeInput([&](const auto &) { ++unrelatedCalls; });
  auto retained = std::make_shared<int>(1);
  std::weak_ptr<int> weakRetained = retained;
  failure = injectedFailure;
  bool failed = false;
  try {
    InputCaptureController controller(registry, profile,
        [retained = std::move(retained)](const InputProfile &, std::string &) { return true; });
    require(injectedFailure == Failure::None, "requested registration failure must propagate");
    require(inputs.size() == 2 && devices.size() == 1, "capture owns both subscriptions while alive");
    const input::PhysicalInputEvent event{
        .control = {.deviceId = "keyboard", .deviceClass = input::DeviceClass::Keyboard,
                    .kind = input::ControlKind::Key},
        .normalizedValue = 0.4F};
    for (const auto &[token, listener] : inputs) listener(event);
    require(controller.monitorSample().has_value(), "live registry callback reaches the real resolver");
  } catch (const std::runtime_error &error) {
    failed = true;
    require(std::string(error.what()) == "injected registry failure", "construction preserves original error");
  }
  failure = Failure::None;
  require(failed == (injectedFailure != Failure::None), "only injected construction fails");
  require(inputs.size() == 1 && inputs.contains(unrelated) && devices.empty(),
          "capture construction failure must not retain callbacks to its destroyed resolver");
  require(weakRetained.expired(), "construction or destruction releases saved callback captures");
  const auto before = unrelatedCalls;
  inputs.at(unrelated)({});
  require(unrelatedCalls == before + 1, "rollback preserves unrelated listeners");
  registry.unsubscribe(unrelated);
}

int main() {
  checkConstruction(Failure::Device);
  checkConstruction(Failure::Input);
  checkConstruction(Failure::None);
}
