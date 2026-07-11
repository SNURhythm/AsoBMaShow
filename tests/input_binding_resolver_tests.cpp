#include "input/InputBindingResolver.h"

#include <SDL2/SDL_scancode.h>

#include <cmath>
#include <iostream>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using TransitionBatch = std::vector<input::LogicalInputTransition>;

void require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

input::LogicalAction lane(int index) {
  return {input::LogicalActionKind::Lane, index};
}

input::LogicalAction scratchClockwise() {
  return {input::LogicalActionKind::ScratchClockwise, 0};
}

input::LogicalAction scratchCounterClockwise() {
  return {input::LogicalActionKind::ScratchCounterClockwise, 0};
}

input::PhysicalControl keyControl(std::string deviceId, int scancode) {
  return {.deviceId = std::move(deviceId),
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

input::PhysicalControl axisControl(std::string deviceId, int index,
                                   input::ControlDirection direction) {
  return {.deviceId = std::move(deviceId),
          .deviceClass = input::DeviceClass::Joystick,
          .kind = input::ControlKind::Axis,
          .index = index,
          .direction = direction};
}

input::PhysicalControl hatControl(std::string deviceId, int index,
                                  input::ControlDirection direction) {
  return {.deviceId = std::move(deviceId),
          .deviceClass = input::DeviceClass::GameController,
          .kind = input::ControlKind::Hat,
          .index = index,
          .direction = direction};
}

input::PhysicalControl midiControl(std::string deviceId, int index) {
  return {.deviceId = std::move(deviceId),
          .deviceClass = input::DeviceClass::Midi,
          .kind = input::ControlKind::MidiControl,
          .index = index,
          .direction = input::ControlDirection::Any};
}

input::InputBinding
binding(std::string id, input::InputScope scope, input::LogicalAction action,
        input::PhysicalControl control, float deadZone = 0.0f,
        float activationThreshold = 0.5f, float releaseThreshold = 0.35f,
        bool inverted = false) {
  return {.id = std::move(id),
          .scope = scope,
          .action = action,
          .control = std::move(control),
          .deadZone = deadZone,
          .activationThreshold = activationThreshold,
          .releaseThreshold = releaseThreshold,
          .inverted = inverted};
}

input::PhysicalInputEvent event(input::PhysicalControl control,
                                float normalizedValue,
                                std::uint64_t timestampMicros = 0) {
  return {.control = std::move(control),
          .rawValue = normalizedValue,
          .normalizedValue = normalizedValue,
          .timestampMicros = timestampMicros};
}

input::PhysicalInputEvent keyEvent(std::string deviceId, int scancode,
                                   bool pressed) {
  return event(keyControl(std::move(deviceId), scancode),
               pressed ? 1.0f : 0.0f);
}

input::PhysicalInputEvent buttonEvent(std::string deviceId, int index,
                                      bool pressed) {
  return event(buttonControl(std::move(deviceId), index),
               pressed ? 1.0f : 0.0f);
}

input::PhysicalInputEvent axisEvent(std::string deviceId, int index,
                                    float value) {
  return event(
      axisControl(std::move(deviceId), index, input::ControlDirection::Any),
      value);
}

input::PhysicalInputEvent hatEvent(std::string deviceId, int index,
                                   input::ControlDirection direction,
                                   bool pressed) {
  return event(hatControl(std::move(deviceId), index, direction),
               pressed ? 1.0f : 0.0f);
}

input::PhysicalInputEvent midiControlEvent(std::string deviceId, int index,
                                           float value) {
  return event(midiControl(std::move(deviceId), index), value);
}

struct Recorder {
  std::vector<TransitionBatch> batches;
  std::vector<input::PhysicalInputEvent> monitorSamples;
  std::vector<input::PhysicalInputEvent> captureCandidates;

  InputBindingResolver::Callbacks callbacks() {
    return {
        .onTransitions =
            [this](std::span<const input::LogicalInputTransition> value) {
              batches.emplace_back(value.begin(), value.end());
            },
        .onMonitorSample =
            [this](const input::PhysicalInputEvent &value) {
              monitorSamples.push_back(value);
            },
        .onCaptureCandidate =
            [this](const input::PhysicalInputEvent &value) {
              captureCandidates.push_back(value);
            },
    };
  }
};

void requireTransition(const input::LogicalInputTransition &transition,
                       input::InputScope scope, input::LogicalAction action,
                       bool pressed, float value, std::string_view message) {
  require(transition.scope == scope, std::string(message) + " scope");
  require(transition.action == action, std::string(message) + " action");
  require(transition.pressed == pressed, std::string(message) + " pressed");
  require(std::fabs(transition.value - value) < 0.0001f,
          std::string(message) + " value");
}

void testScopeFilteringAndDigitalTransitions() {
  InputProfile profile{
      .bindings = {
          binding("p1-7", {1, 7}, lane(0),
                  keyControl("keyboard", SDL_SCANCODE_S)),
          binding("p2-7", {2, 7}, lane(1), buttonControl("inactive-pad", 0)),
          binding("p1-5", {1, 5}, lane(2),
                  keyControl("keyboard", SDL_SCANCODE_S)),
      }};
  Recorder recorder;
  InputBindingResolver resolver(profile, {{1, 7}}, recorder.callbacks());

  resolver.consume(keyEvent("keyboard", SDL_SCANCODE_S, true));
  require(recorder.batches.size() == 1 && recorder.batches[0].size() == 1,
          "scoped key press emits one transition batch");
  requireTransition(recorder.batches[0][0], {1, 7}, lane(0), true, 1.0f,
                    "scoped key press");

  resolver.consume(keyEvent("keyboard", SDL_SCANCODE_S, true));
  require(recorder.batches.size() == 1, "digital repeat is suppressed");

  resolver.consume(keyEvent("keyboard", SDL_SCANCODE_S, false));
  require(recorder.batches.size() == 2 && recorder.batches[1].size() == 1,
          "scoped key release emits one transition batch");
  requireTransition(recorder.batches[1][0], {1, 7}, lane(0), false, 0.0f,
                    "scoped key release");

  resolver.consume(keyEvent("keyboard", SDL_SCANCODE_S, false));
  require(recorder.batches.size() == 2,
          "repeated digital release is suppressed");
  require(resolver.activeDeviceClasses() ==
              std::set{input::DeviceClass::Keyboard},
          "active-scope provenance contains only the keyboard category");
}

void testLogicalReferenceCountsAcrossPhysicalBindings() {
  InputProfile profile{.bindings = {
                           binding("keyboard-lane", {1, 7}, lane(3),
                                   keyControl("keyboard", SDL_SCANCODE_F)),
                           binding("controller-lane", {1, 7}, lane(3),
                                   buttonControl("pad:one", 2)),
                       }};
  Recorder recorder;
  InputBindingResolver resolver(profile, {{1, 7}}, recorder.callbacks());

  require(resolver.activeDeviceClasses() ==
              std::set{input::DeviceClass::Keyboard,
                       input::DeviceClass::GameController},
          "provenance exposes categories for every active-scope binding");

  resolver.consume(keyEvent("keyboard", SDL_SCANCODE_F, true));
  require(recorder.batches.size() == 1,
          "first physical binding presses the logical action");
  resolver.consume(buttonEvent("pad:one", 2, true));
  require(recorder.batches.size() == 1,
          "second physical binding increments without another logical press");
  resolver.consume(keyEvent("keyboard", SDL_SCANCODE_F, false));
  require(recorder.batches.size() == 1,
          "first physical release retains the referenced logical action");
  resolver.consume(buttonEvent("pad:one", 2, false));
  require(recorder.batches.size() == 2 && recorder.batches[1].size() == 1,
          "last physical release releases the logical action");
  requireTransition(recorder.batches[1][0], {1, 7}, lane(3), false, 0.0f,
                    "reference-counted release");
}

void testAxisDeadZoneRescalingAndHysteresis() {
  InputProfile profile{
      .bindings = {binding(
          "positive-axis", {1, 7}, scratchClockwise(),
          axisControl("stick:one", 0, input::ControlDirection::Positive), 0.2f,
          0.5f, 0.3f)}};
  Recorder recorder;
  InputBindingResolver resolver(profile, {{1, 7}}, recorder.callbacks());

  resolver.consume(axisEvent("stick:one", 0, 0.2f));
  resolver.consume(axisEvent("stick:one", 0, 0.59f));
  require(recorder.batches.empty(),
          "dead zone and rescaled sub-threshold axis values stay inactive");

  resolver.consume(axisEvent("stick:one", 0, 0.6f));
  require(recorder.batches.size() == 1 && recorder.batches[0].size() == 1,
          "rescaled activation boundary presses the axis binding");
  requireTransition(recorder.batches[0][0], {1, 7}, scratchClockwise(), true,
                    0.5f, "dead-zone-rescaled axis press");

  resolver.consume(axisEvent("stick:one", 0, 0.52f));
  require(recorder.batches.size() == 1,
          "axis remains active between release and activation thresholds");
  resolver.consume(axisEvent("stick:one", 0, 0.43f));
  require(recorder.batches.size() == 2 && recorder.batches[1].size() == 1,
          "axis releases below the rescaled release threshold");
  requireTransition(recorder.batches[1][0], {1, 7}, scratchClockwise(), false,
                    0.0f, "hysteretic axis release");
}

void testAxisInversion() {
  InputProfile profile{
      .bindings = {binding(
          "inverted-axis", {1, 7}, scratchClockwise(),
          axisControl("stick:invert", 1, input::ControlDirection::Positive),
          0.2f, 0.5f, 0.3f, true)}};
  Recorder recorder;
  InputBindingResolver resolver(profile, {{1, 7}}, recorder.callbacks());

  resolver.consume(axisEvent("stick:invert", 1, -0.6f));
  require(recorder.batches.size() == 1 && recorder.batches[0].size() == 1,
          "inversion maps negative travel into the positive binding");
  requireTransition(recorder.batches[0][0], {1, 7}, scratchClockwise(), true,
                    0.5f, "inverted axis press");
}

void testAxisDirectionsReverseInOneBatch() {
  InputProfile profile{
      .bindings = {
          binding(
              "negative-axis", {1, 7}, scratchCounterClockwise(),
              axisControl("stick:turn", 2, input::ControlDirection::Negative)),
          binding(
              "positive-axis", {1, 7}, scratchClockwise(),
              axisControl("stick:turn", 2, input::ControlDirection::Positive)),
      }};
  Recorder recorder;
  InputBindingResolver resolver(profile, {{1, 7}}, recorder.callbacks());

  resolver.consume(axisEvent("stick:turn", 2, -0.8f));
  require(recorder.batches.size() == 1,
          "negative axis direction presses its logical action");
  requireTransition(recorder.batches[0][0], {1, 7}, scratchCounterClockwise(),
                    true, 0.8f, "negative axis press");

  resolver.consume(axisEvent("stick:turn", 2, 0.8f));
  require(recorder.batches.size() == 2 && recorder.batches[1].size() == 2,
          "axis sign reversal emits exactly one two-transition batch");
  requireTransition(recorder.batches[1][0], {1, 7}, scratchCounterClockwise(),
                    false, 0.0f, "axis reversal release");
  requireTransition(recorder.batches[1][1], {1, 7}, scratchClockwise(), true,
                    0.8f, "axis reversal press");

  resolver.consume(axisEvent("stick:turn", 2, 0.0f));
  require(recorder.batches.size() == 3 && recorder.batches[2].size() == 1,
          "centered axis releases the active direction");
}

void testHatDirections() {
  InputProfile profile{
      .bindings = {
          binding("hat-up", {1, 7}, lane(4),
                  hatControl("pad:hat", 0, input::ControlDirection::Up)),
          binding("hat-right", {1, 7}, lane(5),
                  hatControl("pad:hat", 0, input::ControlDirection::Right)),
      }};
  Recorder recorder;
  InputBindingResolver resolver(profile, {{1, 7}}, recorder.callbacks());

  resolver.consume(hatEvent("pad:hat", 0, input::ControlDirection::Up, true));
  require(recorder.batches.size() == 1,
          "hat direction presses its matching binding");
  requireTransition(recorder.batches[0][0], {1, 7}, lane(4), true, 1.0f,
                    "hat up press");

  resolver.consume(
      hatEvent("pad:hat", 0, input::ControlDirection::Right, true));
  require(recorder.batches.size() == 2 && recorder.batches[1].size() == 1,
          "a second hat direction can be held for a diagonal");
  requireTransition(recorder.batches[1][0], {1, 7}, lane(5), true, 1.0f,
                    "hat right press");

  resolver.consume(
      hatEvent("pad:hat", 0, input::ControlDirection::Right, false));
  require(recorder.batches.size() == 3,
          "hat release affects only its matching direction");
  requireTransition(recorder.batches[2][0], {1, 7}, lane(5), false, 0.0f,
                    "hat right release");

  resolver.consume(
      hatEvent("pad:hat", 0, input::ControlDirection::Right, true));
  require(recorder.batches.size() == 4,
          "released diagonal direction can be pressed again");

  resolver.consume(hatEvent("pad:hat", 0, input::ControlDirection::Any, false));
  require(recorder.batches.size() == 5 && recorder.batches[4].size() == 2,
          "centered hat releases every held direction in one batch");
  requireTransition(recorder.batches[4][0], {1, 7}, lane(4), false, 0.0f,
                    "centered hat up release");
  requireTransition(recorder.batches[4][1], {1, 7}, lane(5), false, 0.0f,
                    "centered hat right release");
}

void testAnyHatBindingTracksEveryHeldDirection() {
  InputProfile profile{
      .bindings = {
          binding("hat-any", {1, 7}, lane(4),
                  hatControl("pad:any-hat", 0, input::ControlDirection::Any))}};
  Recorder recorder;
  InputBindingResolver resolver(profile, {{1, 7}}, recorder.callbacks());

  resolver.consume(
      hatEvent("pad:any-hat", 0, input::ControlDirection::Up, true));
  require(recorder.batches.size() == 1,
          "first any-hat direction presses its logical action");
  resolver.consume(
      hatEvent("pad:any-hat", 0, input::ControlDirection::Right, true));
  require(recorder.batches.size() == 1,
          "second any-hat direction does not repeat the logical press");
  resolver.consume(
      hatEvent("pad:any-hat", 0, input::ControlDirection::Right, false));
  require(recorder.batches.size() == 1,
          "releasing one diagonal direction preserves the any-hat hold");
  resolver.consume(
      hatEvent("pad:any-hat", 0, input::ControlDirection::Up, false));
  require(recorder.batches.size() == 2 && recorder.batches.back().size() == 1,
          "last any-hat direction releases its logical action");
  requireTransition(recorder.batches.back().front(), {1, 7}, lane(4), false,
                    0.0f, "any-hat final release");

  resolver.consume(
      hatEvent("pad:any-hat", 0, input::ControlDirection::Up, true));
  resolver.consume(
      hatEvent("pad:any-hat", 0, input::ControlDirection::Right, true));
  resolver.consume(
      hatEvent("pad:any-hat", 0, input::ControlDirection::Any, false));
  require(recorder.batches.size() == 4,
          "centered any-hat sample releases all held directions once");
  requireTransition(recorder.batches.back().front(), {1, 7}, lane(4), false,
                    0.0f, "any-hat centered release");
}

void testNegativeAxisDeadZoneAndHysteresis() {
  InputProfile profile{
      .bindings = {binding(
          "negative-axis-detailed", {1, 7}, scratchCounterClockwise(),
          axisControl("stick:negative", 3, input::ControlDirection::Negative),
          0.2f, 0.5f, 0.3f)}};
  Recorder recorder;
  InputBindingResolver resolver(profile, {{1, 7}}, recorder.callbacks());

  resolver.consume(axisEvent("stick:negative", 3, -0.59f));
  require(recorder.batches.empty(),
          "negative axis stays inactive below rescaled activation");
  resolver.consume(axisEvent("stick:negative", 3, -0.6f));
  require(recorder.batches.size() == 1,
          "negative axis activates at rescaled threshold");
  requireTransition(recorder.batches[0][0], {1, 7}, scratchCounterClockwise(),
                    true, 0.5f, "negative dead-zone-rescaled press");
  resolver.consume(axisEvent("stick:negative", 3, -0.52f));
  require(recorder.batches.size() == 1,
          "negative axis remains active inside hysteresis band");
  resolver.consume(axisEvent("stick:negative", 3, -0.43f));
  require(recorder.batches.size() == 2,
          "negative axis releases at rescaled release boundary");
}

void testMidiControlThresholdAndHysteresis() {
  constexpr int midiIndex = 3 * 128 + 74;
  InputProfile profile{.bindings = {binding("midi-cc", {1, 7}, lane(6),
                                            midiControl("midi:knob", midiIndex),
                                            0.0f, 0.75f, 0.5f)}};
  Recorder recorder;
  InputBindingResolver resolver(profile, {{1, 7}}, recorder.callbacks());

  resolver.consume(midiControlEvent("midi:knob", midiIndex, 0.74f));
  require(recorder.batches.empty(), "MIDI CC stays inactive below threshold");
  resolver.consume(midiControlEvent("midi:knob", midiIndex, 0.75f));
  require(recorder.batches.size() == 1,
          "MIDI CC activates at its configured threshold");
  resolver.consume(midiControlEvent("midi:knob", midiIndex, 0.6f));
  require(recorder.batches.size() == 1,
          "MIDI CC hysteresis suppresses intermediate changes");
  resolver.consume(midiControlEvent("midi:knob", midiIndex, 0.5f));
  require(recorder.batches.size() == 2,
          "MIDI CC releases at its configured release threshold");
}

void testDisconnectReleasesOnlyUnreferencedActions() {
  InputProfile profile{.bindings = {
                           binding("pad-shared", {1, 7}, lane(1),
                                   buttonControl("pad:disconnect", 0)),
                           binding("keyboard-shared", {1, 7}, lane(1),
                                   keyControl("keyboard", SDL_SCANCODE_D)),
                           binding("pad-only", {1, 7}, lane(2),
                                   buttonControl("pad:disconnect", 1)),
                       }};
  Recorder recorder;
  InputBindingResolver resolver(profile, {{1, 7}}, recorder.callbacks());

  resolver.consume(buttonEvent("pad:disconnect", 0, true));
  resolver.consume(keyEvent("keyboard", SDL_SCANCODE_D, true));
  resolver.consume(buttonEvent("pad:disconnect", 1, true));
  require(recorder.batches.size() == 2,
          "two distinct logical actions are active before disconnect");

  resolver.disconnectDevice("pad:disconnect");
  require(recorder.batches.size() == 3 && recorder.batches[2].size() == 1,
          "disconnect releases only actions no longer referenced");
  requireTransition(recorder.batches[2][0], {1, 7}, lane(2), false, 0.0f,
                    "disconnect-only release");

  resolver.disconnectDevice("pad:disconnect");
  require(recorder.batches.size() == 3,
          "repeated disconnect does not repeat releases");
  resolver.disconnectDevice("keyboard");
  require(recorder.batches.size() == 4 && recorder.batches[3].size() == 1,
          "disconnecting the last reference releases the shared action");
  requireTransition(recorder.batches[3][0], {1, 7}, lane(1), false, 0.0f,
                    "disconnect shared release");
}

void testCaptureModeAndReset() {
  const auto sample = keyEvent("keyboard", SDL_SCANCODE_L, true);
  InputProfile profile{
      .bindings = {binding("capture-key", {1, 7}, lane(6),
                           keyControl("keyboard", SDL_SCANCODE_L))}};
  Recorder recorder;
  InputBindingResolver resolver(profile, {{1, 7}}, recorder.callbacks());

  resolver.setMode(InputBindingResolver::Mode::Capture);
  resolver.consume(sample);
  require(recorder.batches.empty(),
          "capture mode emits no gameplay transitions");
  require(recorder.monitorSamples.size() == 1 &&
              recorder.monitorSamples[0].control == sample.control,
          "capture mode emits the monitor sample");
  require(recorder.captureCandidates.size() == 1 &&
              recorder.captureCandidates[0].control == sample.control,
          "capture mode emits the capture candidate");

  resolver.setMode(InputBindingResolver::Mode::Gameplay);
  resolver.consume(sample);
  require(recorder.batches.size() == 1,
          "capture sampling does not alter gameplay active state");
  resolver.reset();
  require(recorder.batches.size() == 2 && recorder.batches[1].size() == 1,
          "reset releases active logical actions in one batch");
  requireTransition(recorder.batches[1][0], {1, 7}, lane(6), false, 0.0f,
                    "reset release");
  resolver.reset();
  require(recorder.batches.size() == 2,
          "repeated reset does not repeat releases");
  require(resolver.activeDeviceClasses() ==
              std::set{input::DeviceClass::Keyboard},
          "reset preserves active-scope category provenance");
}

void testCaptureModeReleasesAndReentryAcceptsNextPress() {
  InputProfile profile{
      .bindings = {binding("capture-reentry", {1, 7}, lane(5),
                           keyControl("keyboard", SDL_SCANCODE_K))}};
  Recorder recorder;
  InputBindingResolver resolver(profile, {{1, 7}}, recorder.callbacks());

  resolver.consume(keyEvent("keyboard", SDL_SCANCODE_K, true));
  require(recorder.batches.size() == 1 && recorder.batches.back()[0].pressed,
          "gameplay press is active before capture");
  resolver.setMode(InputBindingResolver::Mode::Capture);
  require(recorder.batches.size() == 2 && !recorder.batches.back()[0].pressed,
          "entering capture releases gameplay-owned logical state");
  resolver.consume(keyEvent("keyboard", SDL_SCANCODE_K, false));
  resolver.setMode(InputBindingResolver::Mode::Gameplay);
  resolver.consume(keyEvent("keyboard", SDL_SCANCODE_K, true));
  require(recorder.batches.size() == 3 && recorder.batches.back()[0].pressed,
          "first post-capture press is not suppressed by stale state");
}
} // namespace

int main() {
  try {
    testScopeFilteringAndDigitalTransitions();
    testLogicalReferenceCountsAcrossPhysicalBindings();
    testAxisDeadZoneRescalingAndHysteresis();
    testAxisInversion();
    testAxisDirectionsReverseInOneBatch();
    testHatDirections();
    testAnyHatBindingTracksEveryHeldDirection();
    testNegativeAxisDeadZoneAndHysteresis();
    testMidiControlThresholdAndHysteresis();
    testDisconnectReleasesOnlyUnreferencedActions();
    testCaptureModeAndReset();
    testCaptureModeReleasesAndReentryAcceptsNextPress();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "input_binding_resolver_tests: " << error.what() << '\n';
    return 1;
  }
}
