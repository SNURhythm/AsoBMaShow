#include "input/IInputBackend.h"
#include "input/InputCaptureController.h"
#include "input/InputDeviceRegistry.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

input::LogicalAction lane(int index) {
  return {input::LogicalActionKind::Lane, index};
}

input::PhysicalControl keyControl(int scancode) {
  return {.deviceId = "keyboard",
          .deviceClass = input::DeviceClass::Keyboard,
          .kind = input::ControlKind::Key,
          .index = scancode,
          .direction = input::ControlDirection::Any};
}

input::PhysicalControl buttonControl(std::string deviceId, int index) {
  return {.deviceId = std::move(deviceId),
          .deviceClass = input::DeviceClass::GameController,
          .kind = input::ControlKind::Button,
          .index = index,
          .direction = input::ControlDirection::Any};
}

input::PhysicalControl axisControl(std::string deviceId, int index) {
  return {.deviceId = std::move(deviceId),
          .deviceClass = input::DeviceClass::Joystick,
          .kind = input::ControlKind::Axis,
          .index = index,
          .direction = input::ControlDirection::Any};
}

input::PhysicalControl hatControl(std::string deviceId, int index,
                                  input::ControlDirection direction) {
  return {.deviceId = std::move(deviceId),
          .deviceClass = input::DeviceClass::Joystick,
          .kind = input::ControlKind::Hat,
          .index = index,
          .direction = direction};
}

input::PhysicalControl midiNoteControl(std::string deviceId, int index) {
  return {.deviceId = std::move(deviceId),
          .deviceClass = input::DeviceClass::Midi,
          .kind = input::ControlKind::MidiNote,
          .index = index,
          .direction = input::ControlDirection::Any};
}

input::InputBinding binding(std::string id, input::InputScope scope,
                            input::LogicalAction action,
                            input::PhysicalControl control) {
  return {.id = std::move(id),
          .scope = scope,
          .action = action,
          .control = std::move(control)};
}

input::PhysicalInputEvent event(input::PhysicalControl control, float value,
                                std::uint64_t timestamp = 0) {
  return {.control = std::move(control),
          .rawValue = value,
          .normalizedValue = value,
          .timestampMicros = timestamp};
}

bool sameBinding(const input::InputBinding &left,
                 const input::InputBinding &right) {
  return left.id == right.id && left.scope == right.scope &&
         left.action == right.action && left.control == right.control &&
         left.deadZone == right.deadZone &&
         left.activationThreshold == right.activationThreshold &&
         left.releaseThreshold == right.releaseThreshold &&
         left.inverted == right.inverted;
}

bool sameProfile(const InputProfile &left, const InputProfile &right) {
  if (left.schemaVersion != right.schemaVersion ||
      left.bindings.size() != right.bindings.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.bindings.size(); ++index) {
    if (!sameBinding(left.bindings[index], right.bindings[index])) {
      return false;
    }
  }
  return true;
}

class FakeBackend final : public IInputBackend {
public:
  explicit FakeBackend(input::InputBackendSink sink)
      : IInputBackend(std::move(sink)) {}

  bool start(std::string &) override { return true; }
  void stop() override {}
  void pump() override {}

  void sendInput(input::PhysicalInputEvent value) {
    publishInput(std::move(value));
  }

  void sendDevice(input::InputDeviceSnapshot value) {
    publishDevice(std::move(value));
  }
};

struct RegistryHarness {
  FakeBackend *backend = nullptr;
  InputDeviceRegistry registry;

  RegistryHarness()
      : registry(std::vector<InputDeviceRegistry::BackendFactory>{
            [this](input::InputBackendSink sink) {
              auto result = std::make_unique<FakeBackend>(std::move(sink));
              backend = result.get();
              return result;
            }}) {}

  void input(input::PhysicalInputEvent value) {
    backend->sendInput(std::move(value));
    registry.pump();
  }

  void device(input::InputDeviceSnapshot value) {
    backend->sendDevice(std::move(value));
    registry.pump();
  }
};

void testMonitoringNoiseActivationRepeatsAndDuplicateIgnore() {
  RegistryHarness harness;
  InputProfile profile;
  int saves = 0;
  InputCaptureController controller(harness.registry, profile,
                                    [&](const InputProfile &, std::string &) {
                                      ++saves;
                                      return true;
                                    });

  controller.begin({1, 7}, lane(0));
  harness.input(event(axisControl("stick:one", 2), 0.08F, 10));
  require(controller.monitorSample().has_value() &&
              controller.monitorSample()->timestampMicros == 10,
          "axis noise remains visible in the live monitor");
  require(controller.state() == InputCaptureController::State::Listening &&
              profile.bindings.empty() && saves == 0,
          "axis noise does not become a binding or persist");

  harness.input(event(axisControl("stick:one", 2), 0.49F, 11));
  require(profile.bindings.empty(),
          "an axis remains unbound below the activation threshold");
  harness.input(event(axisControl("stick:one", 2), 0.50F, 12));
  require(controller.state() == InputCaptureController::State::Idle &&
              profile.bindings.size() == 1 && saves == 1,
          "an axis binds only when it crosses the activation threshold");
  require(profile.bindings.front().control.direction ==
              input::ControlDirection::Positive,
          "a positive axis capture persists its direction");

  controller.begin({1, 7}, lane(1));
  harness.input(event(keyControl(22), 0.0F, 20));
  require(controller.state() == InputCaptureController::State::Listening,
          "a digital release is not a capture candidate");
  harness.input(event(keyControl(22), 1.0F, 21));
  require(profile.bindings.size() == 2 && saves == 2,
          "a digital activation crossing commits one binding");
  harness.input(event(keyControl(22), 1.0F, 22));
  require(profile.bindings.size() == 2 && saves == 2,
          "a repeated digital press cannot commit a second binding");

  harness.input(event(keyControl(22), 0.0F, 23));
  controller.begin({1, 7}, lane(2));
  harness.input(event(keyControl(22), 1.0F, 24));
  require(controller.state() ==
              InputCaptureController::State::AwaitingConflictConfirmation,
          "a release observed while idle re-arms the next activation crossing");
  controller.rejectReplace();

  controller.begin({1, 7}, lane(1));
  harness.input(event(keyControl(22), 1.0F, 25));
  require(controller.state() == InputCaptureController::State::Listening &&
              profile.bindings.size() == 2 && saves == 2,
          "a held-key repeat is ignored while listening");
  harness.input(event(keyControl(22), 0.0F, 26));
  harness.input(event(keyControl(22), 1.0F, 27));
  require(controller.state() == InputCaptureController::State::Listening &&
              profile.bindings.size() == 2 && saves == 2,
          "an exact scoped action/control duplicate is ignored without save");
  controller.cancel();
}

void testConflictConfirmationIsTransactionalAndScopeLimited() {
  RegistryHarness harness;
  const auto occupied =
      binding("occupied", {1, 7}, lane(0), buttonControl("pad:one", 3));
  const auto otherScope =
      binding("other-scope", {2, 7}, lane(2), buttonControl("pad:one", 3));
  const auto otherControl =
      binding("other-control", {1, 7}, lane(3), buttonControl("pad:one", 4));
  InputProfile profile{.bindings = {occupied, otherScope, otherControl}};
  const InputProfile before = profile;
  int saves = 0;
  InputCaptureController controller(harness.registry, profile,
                                    [&](const InputProfile &, std::string &) {
                                      ++saves;
                                      return true;
                                    });

  controller.begin({1, 7}, lane(1));
  harness.input(event(buttonControl("pad:one", 3), 1.0F));
  require(controller.state() ==
              InputCaptureController::State::AwaitingConflictConfirmation,
          "a conflicting capture waits for explicit confirmation");
  require(controller.pendingConflicts().size() == 1 &&
              controller.pendingConflicts().front().id == "occupied",
          "only the same-scope conflict is presented");
  require(sameProfile(profile, before) && saves == 0,
          "conflict discovery neither mutates nor persists the profile");

  controller.rejectReplace();
  require(controller.state() == InputCaptureController::State::Idle &&
              sameProfile(profile, before) && saves == 0,
          "rejecting replacement preserves the complete profile");

  controller.begin({1, 7}, lane(1));
  harness.input(event(buttonControl("pad:one", 3), 0.0F));
  harness.input(event(buttonControl("pad:one", 3), 1.0F));
  controller.confirmReplace();
  require(controller.state() == InputCaptureController::State::Idle &&
              saves == 1,
          "confirming replacement persists exactly once");
  require(profile.bindings.size() == 3,
          "replacement removes one scoped conflict and adds one binding");
  require(std::ranges::any_of(profile.bindings,
                              [&](const auto &value) {
                                return sameBinding(value, otherScope);
                              }),
          "replacement retains the identical control in another scope");
  require(std::ranges::any_of(profile.bindings,
                              [&](const auto &value) {
                                return sameBinding(value, otherControl);
                              }),
          "replacement retains non-conflicting controls in the same scope");
  require(std::ranges::none_of(
              profile.bindings,
              [&](const auto &value) { return value.id == occupied.id; }),
          "replacement removes the confirmed same-scope conflict");
  require(std::ranges::any_of(profile.bindings,
                              [&](const auto &value) {
                                return value.scope == input::InputScope{1, 7} &&
                                       value.action == lane(1) &&
                                       value.control ==
                                           buttonControl("pad:one", 3);
                              }),
          "replacement installs the captured action");
}

void testSaveFailureNeverCommitsProfileMutation() {
  RegistryHarness harness;
  InputProfile profile;
  int saves = 0;
  InputCaptureController controller(
      harness.registry, profile, [&](const InputProfile &, std::string &error) {
        ++saves;
        error = "injected save failure";
        return false;
      });

  controller.begin({1, 7}, lane(0));
  harness.input(event(keyControl(7), 1.0F));
  require(profile.bindings.empty() && saves == 1,
          "a failed capture save leaves the live profile unchanged");
  require(controller.state() == InputCaptureController::State::Listening &&
              controller.lastError() == "injected save failure",
          "a failed capture remains retryable and exposes its error");
}

void testMissingStableIdsRemainVisibleAcrossHotplug() {
  RegistryHarness harness;
  InputProfile profile{
      .bindings = {
          binding("missing", {1, 7}, lane(0), buttonControl("pad:missing", 1)),
          binding("blank", {1, 7}, lane(1), buttonControl("", 2)),
          binding("keyboard", {1, 7}, lane(2), keyControl(4)),
      }};
  InputCaptureController controller(
      harness.registry, profile,
      [](const InputProfile &, std::string &) { return true; });

  harness.registry.pump();
  require(controller.isBindingDeviceMissing("missing"),
          "a disconnected saved stable ID is reported as missing");
  require(controller.isBindingDeviceMissing("blank"),
          "an imported blank stable ID is reported as missing");
  require(!controller.isBindingDeviceMissing("keyboard"),
          "the registry's permanent keyboard is not reported missing");

  harness.device({.stableId = "pad:missing",
                  .displayName = "Pad",
                  .deviceClass = input::DeviceClass::GameController,
                  .connected = true});
  require(!controller.isBindingDeviceMissing("missing"),
          "a matching connected stable ID immediately restores visibility");
  harness.device({.stableId = "pad:missing",
                  .displayName = "Pad",
                  .deviceClass = input::DeviceClass::GameController,
                  .connected = false});
  require(controller.isBindingDeviceMissing("missing"),
          "disconnect preserves the binding and reports its saved stable ID");
}

void testSanitizedEditsAndScopedResetPersistOnlyCommittedChanges() {
  RegistryHarness harness;
  auto editable =
      binding("editable", {1, 7}, lane(0), buttonControl("pad:one", 0));
  auto untouched =
      binding("untouched", {2, 14}, lane(8), buttonControl("pad:two", 1));
  untouched.deadZone = 0.1F;
  untouched.releaseThreshold = 0.25F;
  untouched.activationThreshold = 0.7F;
  InputProfile profile{.bindings = {editable, untouched}};
  int saves = 0;
  std::vector<InputProfile> savedProfiles;
  InputCaptureController controller(
      harness.registry, profile, [&](const InputProfile &value, std::string &) {
        ++saves;
        savedProfiles.push_back(value);
        return true;
      });

  controller.updateBinding("does-not-exist", {.deadZone = 0.1F,
                                              .activationThreshold = 0.8F,
                                              .releaseThreshold = 0.4F,
                                              .inverted = true});
  require(saves == 0, "editing an unknown binding is a no-op");

  controller.updateBinding("editable",
                           {.deadZone = std::numeric_limits<float>::quiet_NaN(),
                            .activationThreshold = 0.2F,
                            .releaseThreshold = 0.9F,
                            .inverted = true});
  require(saves == 1 && savedProfiles.size() == 1,
          "a committed binding edit persists once");
  const auto &edited = profile.bindings.front();
  require(edited.deadZone == 0.0F && edited.releaseThreshold == 0.35F &&
              edited.activationThreshold == 0.5F && edited.inverted,
          "invalid threshold edits are sanitized before persistence");
  require(sameBinding(profile.bindings[1], untouched),
          "sanitizing one edit does not rewrite an unrelated binding");

  controller.updateBinding("editable",
                           {.deadZone = edited.deadZone,
                            .activationThreshold = edited.activationThreshold,
                            .releaseThreshold = edited.releaseThreshold,
                            .inverted = edited.inverted});
  require(saves == 1,
          "an edit with no effective committed change does not persist");

  controller.resetScopeToDefaults({1, 7});
  require(saves == 2, "an explicit scoped reset persists exactly once");
  const InputProfile defaults = makeDefaultInputProfile();
  const auto expectedDefaults = defaults.bindingsFor({1, 7});
  const auto actualDefaults = profile.bindingsFor({1, 7});
  require(actualDefaults.size() == expectedDefaults.size(),
          "reset installs every current default for only the selected scope");
  for (const auto &expected : expectedDefaults) {
    require(std::ranges::any_of(actualDefaults,
                                [&](const auto &actual) {
                                  return sameBinding(actual.get(),
                                                     expected.get());
                                }),
            "reset scope matches the generated keyboard defaults");
  }
  require(std::ranges::any_of(
              profile.bindings,
              [&](const auto &value) { return sameBinding(value, untouched); }),
          "scoped reset preserves every binding in other scopes");
}

void testPartialBindingEditsComposeAgainstCurrentProfileState() {
  RegistryHarness harness;
  auto editable =
      binding("editable", {1, 7}, lane(0), buttonControl("pad:one", 0));
  InputProfile profile{.bindings = {editable}};
  int saves = 0;
  InputCaptureController controller(harness.registry, profile,
                                    [&](const InputProfile &, std::string &) {
                                      ++saves;
                                      return true;
                                    });

  controller.updateBinding("editable", {.deadZone = 0.1F});
  controller.toggleBindingInversion("editable");
  require(profile.bindings.front().deadZone == 0.1F &&
              profile.bindings.front().inverted,
          "an inversion click composes with the threshold committed by "
          "pointer-down focus loss");
  controller.toggleBindingInversion("editable");
  controller.toggleBindingInversion("editable");
  controller.updateBinding("editable", {.releaseThreshold = 0.3F});

  const auto &edited = profile.bindings.front();
  require(saves == 5, "each committed partial edit persists exactly once");
  require(edited.deadZone == 0.1F && edited.releaseThreshold == 0.3F &&
              edited.activationThreshold == editable.activationThreshold &&
              edited.inverted,
          "sibling edits and two rapid inversion toggles compose against the "
          "latest binding instead of a stale snapshot");
}

void testAmbiguousBindingIdsFailClosed() {
  RegistryHarness harness;
  auto first =
      binding("duplicate", {1, 7}, lane(0), buttonControl("pad:missing", 0));
  auto second = binding("duplicate", {1, 7}, lane(1), keyControl(4));
  InputProfile profile{.bindings = {first, second}};
  const InputProfile before = profile;
  int saves = 0;
  InputCaptureController controller(harness.registry, profile,
                                    [&](const InputProfile &, std::string &) {
                                      ++saves;
                                      return true;
                                    });

  controller.updateBinding("duplicate", {.inverted = true});
  require(saves == 0 && sameProfile(profile, before),
          "an ambiguous binding edit fails closed without persistence or "
          "mutation");
  require(!controller.lastError().empty(),
          "an ambiguous binding edit exposes a diagnostic");
  require(!controller.isBindingDeviceMissing("duplicate"),
          "an ambiguous missing-device lookup fails closed rather than "
          "selecting the first row");
}

void testDisconnectRearmsFirstCaptureAfterReconnect() {
  RegistryHarness harness;
  InputProfile profile;
  int saves = 0;
  InputCaptureController controller(harness.registry, profile,
                                    [&](const InputProfile &, std::string &) {
                                      ++saves;
                                      return true;
                                    });

  harness.device({.stableId = "pad:one",
                  .displayName = "Pad",
                  .deviceClass = input::DeviceClass::GameController,
                  .connected = true});
  harness.input(event(buttonControl("pad:one", 3), 1.0F));
  harness.device({.stableId = "pad:one",
                  .displayName = "Pad",
                  .deviceClass = input::DeviceClass::GameController,
                  .connected = false});
  harness.device({.stableId = "pad:one",
                  .displayName = "Pad",
                  .deviceClass = input::DeviceClass::GameController,
                  .connected = true});

  controller.begin({1, 7}, lane(0));
  harness.input(event(buttonControl("pad:one", 3), 1.0F));
  require(controller.state() == InputCaptureController::State::Idle &&
              profile.bindings.size() == 1 && saves == 1,
          "disconnect clears the held edge so the first press after reconnect "
          "captures without a synthetic release");
}

void testNegativeAxisHatAndMidiCapturePreserveControlIdentity() {
  RegistryHarness harness;
  InputProfile profile;
  int saves = 0;
  InputCaptureController controller(harness.registry, profile,
                                    [&](const InputProfile &, std::string &) {
                                      ++saves;
                                      return true;
                                    });

  controller.begin({1, 7}, lane(0));
  harness.input(event(axisControl("stick:one", 2), -0.49F));
  require(profile.bindings.empty(),
          "negative axis noise below the threshold remains unbound");
  harness.input(event(axisControl("stick:one", 2), -0.5F));
  require(profile.bindings.back().control.direction ==
              input::ControlDirection::Negative,
          "negative axis capture persists its sign-specific direction");

  controller.begin({1, 7}, lane(1));
  harness.input(
      event(hatControl("stick:one", 0, input::ControlDirection::Up), 1.0F));
  require(profile.bindings.back().control.kind == input::ControlKind::Hat &&
              profile.bindings.back().control.direction ==
                  input::ControlDirection::Up,
          "hat capture preserves its discrete direction");

  controller.begin({1, 7}, lane(2));
  harness.input(event(midiNoteControl("midi:one", 2 * 128 + 60), 1.0F));
  require(
      profile.bindings.back().control.kind == input::ControlKind::MidiNote &&
          profile.bindings.back().control.index == 2 * 128 + 60 && saves == 3,
      "MIDI capture preserves channel-note identity and saves once");
}

} // namespace

int main() {
  try {
    testMonitoringNoiseActivationRepeatsAndDuplicateIgnore();
    testConflictConfirmationIsTransactionalAndScopeLimited();
    testSaveFailureNeverCommitsProfileMutation();
    testMissingStableIdsRemainVisibleAcrossHotplug();
    testSanitizedEditsAndScopedResetPersistOnlyCommittedChanges();
    testPartialBindingEditsComposeAgainstCurrentProfileState();
    testAmbiguousBindingIdsFailClosed();
    testDisconnectRearmsFirstCaptureAfterReconnect();
    testNegativeAxisHatAndMidiCapturePreserveControlIdentity();
  } catch (const std::exception &error) {
    std::cerr << "input_capture_controller_tests: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
