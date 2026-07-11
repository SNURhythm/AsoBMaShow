#include "input/IInputBackend.h"
#include "input/InputCaptureController.h"
#include "input/InputDeviceRegistry.h"
#include "input/InputProfileReplacementNotifier.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

input::PhysicalControl midiControlControl(std::string deviceId, int index) {
  return {.deviceId = std::move(deviceId),
          .deviceClass = input::DeviceClass::Midi,
          .kind = input::ControlKind::MidiControl,
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
      left.gyroscopeTurntable != right.gyroscopeTurntable ||
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

void testGyroscopeConfigUpdateIsSanitizedAndTransactional() {
  RegistryHarness harness;
  InputProfile profile;
  int saves = 0;
  int runtimeApplications = 0;
  bool allowSave = true;
  input::GyroscopeTurntableConfig runtimeConfig = profile.gyroscopeTurntable;
  std::vector<InputProfile> candidates;
  InputCaptureController controller(
      harness.registry, profile,
      [&](const InputProfile &candidate, std::string &error) {
        ++saves;
        candidates.push_back(candidate);
        if (allowSave) {
          runtimeConfig = candidate.gyroscopeTurntable;
          ++runtimeApplications;
          return true;
        }
        error = "injected gyroscope save failure";
        return false;
      });

  require(controller.updateGyroscopeTurntableConfig(
              {.stepAngleDegrees = 999, .releaseDelayMs = -1}),
          "a valid gyroscope configuration edit is committed");
  require(saves == 1 && candidates.size() == 1 &&
              profile.gyroscopeTurntable.stepAngleDegrees == 45 &&
              profile.gyroscopeTurntable.releaseDelayMs == 50 &&
              candidates.front().gyroscopeTurntable ==
                  profile.gyroscopeTurntable &&
              runtimeConfig == profile.gyroscopeTurntable &&
              runtimeApplications == 1,
          "gyroscope configuration is sanitized and applied after successful "
          "persistence");

  require(controller.updateGyroscopeTurntableConfig(
              {.stepAngleDegrees = 1000, .releaseDelayMs = 0}) &&
              saves == 1 && runtimeApplications == 1,
          "an edit with the same sanitized configuration is a successful "
          "no-op");

  const InputProfile beforeFailure = profile;
  allowSave = false;
  require(!controller.updateGyroscopeTurntableConfig(
              {.stepAngleDegrees = 6, .releaseDelayMs = 400}),
          "a failed gyroscope configuration save reports failure");
  require(saves == 2 && sameProfile(profile, beforeFailure) &&
              runtimeConfig == beforeFailure.gyroscopeTurntable &&
              runtimeApplications == 1 &&
              controller.lastError() == "injected gyroscope save failure",
          "a failed gyroscope configuration save leaves the live profile and "
          "runtime unchanged");
}

void testRuntimeSaveAppliesOnlyChangedGyroscopeConfigAfterSuccess() {
  InputProfile current = makeDefaultInputProfile();
  InputProfile bindingOnly = current;
  bindingOnly.bindings.push_back(
      binding("runtime-guard", {1, 7}, lane(0), keyControl(12)));
  int saves = 0;
  int applications = 0;
  input::GyroscopeTurntableConfig runtimeConfig = current.gyroscopeTurntable;
  std::string error;

  require(input_profile_runtime::saveThenApplyGyroscopeConfig(
              current, bindingOnly,
              [&](const InputProfile &, std::string &) {
                ++saves;
                return true;
              },
              [&](input::GyroscopeTurntableConfig config) {
                ++applications;
                runtimeConfig = config;
              },
              error) &&
              saves == 1 && applications == 0 &&
              runtimeConfig == current.gyroscopeTurntable,
          "a binding-only save does not reset the gyroscope runtime");

  InputProfile changed = current;
  changed.gyroscopeTurntable =
      {.stepAngleDegrees = 7, .releaseDelayMs = 350};
  require(!input_profile_runtime::saveThenApplyGyroscopeConfig(
              current, changed,
              [&](const InputProfile &, std::string &saveError) {
                ++saves;
                saveError = "injected runtime save failure";
                return false;
              },
              [&](input::GyroscopeTurntableConfig) { ++applications; },
              error) &&
              saves == 2 && applications == 0,
          "a failed save never applies changed gyroscope configuration");

  require(input_profile_runtime::saveThenApplyGyroscopeConfig(
              current, changed,
              [&](const InputProfile &, std::string &) {
                ++saves;
                return true;
              },
              [&](input::GyroscopeTurntableConfig config) {
                ++applications;
                runtimeConfig = config;
              },
              error) &&
              saves == 3 && applications == 1 &&
              runtimeConfig == changed.gyroscopeTurntable,
          "a changed gyroscope configuration applies once after its save");
}

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

  harness.input(event(axisControl("stick:one", 2), 0.19F, 11));
  require(profile.bindings.empty(),
          "an axis remains unbound below the sensitive activation threshold");
  harness.input(event(axisControl("stick:one", 2), 0.20F, 12));
  require(controller.state() == InputCaptureController::State::Idle &&
              profile.bindings.size() == 1 && saves == 1,
          "an axis binds at the sensitive activation threshold");
  require(profile.bindings.front().activationThreshold == 0.20F &&
              profile.bindings.front().releaseThreshold == 0.10F,
          "captured axes inherit the sensitive gameplay thresholds");
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

void testAxisCaptureUsesSensitiveHysteresis() {
  RegistryHarness harness;
  InputProfile profile;
  int saves = 0;
  InputCaptureController controller(harness.registry, profile,
                                    [&](const InputProfile &, std::string &) {
                                      ++saves;
                                      return true;
                                    });
  const auto axis = axisControl("gyro:one", 1);

  controller.begin({1, 7}, lane(0));
  harness.input(event(axis, 0.20F));
  require(profile.bindings.size() == 1 && saves == 1,
          "light deliberate gyro motion creates a binding");

  controller.begin({1, 7}, lane(1));
  harness.input(event(axis, 0.15F));
  harness.input(event(axis, 0.20F));
  require(controller.state() == InputCaptureController::State::Listening &&
              saves == 1,
          "mid-band gyro noise does not re-arm an active direction");

  harness.input(event(axis, 0.10F));
  harness.input(event(axis, 0.20F));
  require(controller.state() ==
              InputCaptureController::State::AwaitingConflictConfirmation,
          "returning to the release threshold re-arms the gyro direction");
  controller.rejectReplace();
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
  require(edited.deadZone == 0.0F && edited.releaseThreshold == 0.10F &&
              edited.activationThreshold == 0.20F && edited.inverted,
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

  controller.updateBinding("editable", {.deadZone = 0.05F});
  controller.toggleBindingInversion("editable");
  require(profile.bindings.front().deadZone == 0.05F &&
              profile.bindings.front().inverted,
          "an inversion click composes with the threshold committed by "
          "pointer-down focus loss");
  controller.toggleBindingInversion("editable");
  controller.toggleBindingInversion("editable");
  controller.updateBinding("editable", {.releaseThreshold = 0.15F});

  const auto &edited = profile.bindings.front();
  require(saves == 5, "each committed partial edit persists exactly once");
  require(edited.deadZone == 0.05F && edited.releaseThreshold == 0.15F &&
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
  harness.input(event(axisControl("stick:one", 2), -0.19F));
  require(profile.bindings.empty(),
          "negative axis noise below the threshold remains unbound");
  harness.input(event(axisControl("stick:one", 2), -0.20F));
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

void testNonAxisCaptureThresholdRemainsUnchanged() {
  RegistryHarness harness;
  InputProfile profile;
  InputCaptureController controller(harness.registry, profile,
                                    [](const InputProfile &, std::string &) {
                                      return true;
                                    });

  controller.begin({1, 7}, lane(0));
  harness.input(event(midiControlControl("midi:one", 7), 0.20F));
  require(profile.bindings.empty() &&
              controller.state() == InputCaptureController::State::Listening,
          "MIDI control capture does not inherit the axis threshold");
  harness.input(event(midiControlControl("midi:one", 7), 0.50F));
  require(profile.bindings.size() == 1,
          "MIDI control capture retains the existing threshold");
}

void testProfileReplacementCancelsPendingConflictAndRegistrationLifetime() {
  RegistryHarness harness;
  const auto occupied =
      binding("profile-a-binding", {1, 7}, lane(0), keyControl(30));
  InputProfile profile{.bindings = {occupied}};
  int saves = 0;
  InputCaptureController controller(harness.registry, profile,
                                    [&](const InputProfile &, std::string &) {
                                      ++saves;
                                      return true;
                                    });
  InputProfileReplacementNotifier notifier;

  {
    auto registration = notifier.subscribe([&controller]() {
      controller.cancel();
    });
    controller.begin({1, 7}, lane(1));
    harness.input(event(keyControl(30), 1.0F));
    require(controller.state() ==
                InputCaptureController::State::AwaitingConflictConfirmation,
            "profile A can stage a conflicting capture before a switch");

    notifier.notifyBeforeReplacement();
    require(controller.state() == InputCaptureController::State::Idle,
            "the replacement notification cancels profile A's staged "
            "capture");

    const InputProfile profileB{
        .bindings = {binding("profile-b-binding", {1, 7}, lane(2),
                             keyControl(31))}};
    profile = profileB;
    controller.confirmReplace();
    require(saves == 0 && sameProfile(profile, profileB),
            "confirming after the switch cannot save or mutate profile B");
  }

  controller.begin({1, 7}, lane(3));
  notifier.notifyBeforeReplacement();
  require(controller.state() == InputCaptureController::State::Listening,
          "destroying the scoped registration removes its callback");
  controller.cancel();
}

void testReplacementNotifierIsReentrantAndResetIsALifetimeBarrier() {
  InputProfileReplacementNotifier notifier;
  InputProfileReplacementNotifier::Registration registration;
  int calls = 0;
  registration = notifier.subscribe([&]() {
    ++calls;
    auto nested = notifier.subscribe([]() {});
    nested.reset();
    registration.reset();
  });
  notifier.notifyBeforeReplacement();
  notifier.notifyBeforeReplacement();
  require(calls == 1,
          "a callback can subscribe and unregister itself without deadlock");

  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  bool callbackEntered = false;
  bool releaseCallback = false;
  bool resetStarted = false;
  std::atomic<bool> resetFinished{false};
  std::atomic<int> barrierCalls{0};
  auto barrierRegistration = notifier.subscribe([&]() {
    barrierCalls.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock(callbackMutex);
    callbackEntered = true;
    callbackCondition.notify_all();
    callbackCondition.wait(lock, [&]() { return releaseCallback; });
  });

  std::thread notifying([&]() { notifier.notifyBeforeReplacement(); });
  {
    std::unique_lock lock(callbackMutex);
    const bool entered = callbackCondition.wait_for(
        lock, std::chrono::seconds(2), [&]() { return callbackEntered; });
    if (!entered) {
      releaseCallback = true;
      callbackCondition.notify_all();
      lock.unlock();
      notifying.join();
      require(false, "lifetime-barrier callback starts");
    }
  }

  std::thread resetting(
      [registration = std::move(barrierRegistration), &callbackMutex,
       &callbackCondition, &resetStarted, &resetFinished]() mutable {
        {
          const std::lock_guard lock(callbackMutex);
          resetStarted = true;
        }
        callbackCondition.notify_all();
        registration.reset();
        resetFinished.store(true, std::memory_order_release);
      });
  {
    std::unique_lock lock(callbackMutex);
    callbackCondition.wait(lock, [&]() { return resetStarted; });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  const bool resetReturnedEarly =
      resetFinished.load(std::memory_order_acquire);

  {
    const std::lock_guard lock(callbackMutex);
    releaseCallback = true;
  }
  callbackCondition.notify_all();
  notifying.join();
  resetting.join();
  require(!resetReturnedEarly,
          "cross-thread reset waits for the in-flight callback");
  require(resetFinished.load(std::memory_order_acquire),
          "reset returns after the in-flight callback exits");

  notifier.notifyBeforeReplacement();
  require(barrierCalls.load(std::memory_order_relaxed) == 1,
          "a completed reset prevents every later callback");
}

} // namespace

int main() {
  try {
    testMonitoringNoiseActivationRepeatsAndDuplicateIgnore();
    testGyroscopeConfigUpdateIsSanitizedAndTransactional();
    testRuntimeSaveAppliesOnlyChangedGyroscopeConfigAfterSuccess();
    testAxisCaptureUsesSensitiveHysteresis();
    testConflictConfirmationIsTransactionalAndScopeLimited();
    testSaveFailureNeverCommitsProfileMutation();
    testMissingStableIdsRemainVisibleAcrossHotplug();
    testSanitizedEditsAndScopedResetPersistOnlyCommittedChanges();
    testPartialBindingEditsComposeAgainstCurrentProfileState();
    testAmbiguousBindingIdsFailClosed();
    testDisconnectRearmsFirstCaptureAfterReconnect();
    testNegativeAxisHatAndMidiCapturePreserveControlIdentity();
    testNonAxisCaptureThresholdRemainsUnchanged();
    testProfileReplacementCancelsPendingConflictAndRegistrationLifetime();
    testReplacementNotifierIsReentrantAndResetIsALifetimeBarrier();
  } catch (const std::exception &error) {
    std::cerr << "input_capture_controller_tests: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
