#pragma once

#include "../input/GyroscopeTurntable.h"
#include "../input/InputTypes.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace settings_scene {

inline constexpr std::string_view kGyroscopeStepAngleLabel = "Step angle (°)";
inline constexpr std::string_view kGyroscopeReleaseDelayLabel =
    "Release delay (ms)";

constexpr bool shouldShowGyroscopeSettingsCard(std::string_view stableId) {
  return stableId == input::kGyroscopeTurntableStableId;
}

inline std::string gyroscopeSettingsErrorLabel(std::string_view error) {
  return error.empty() ? std::string() : "Not saved: " + std::string(error);
}

struct InputSettingsLayout {
  bool stackSelectors = false;
  bool stackBindingEditor = false;
  int selectorGap = 12;
  int actionGroupPadding = 16;
  int selectorWidth = 0;
  int bindingEditorWidth = 0;
};

struct InputBindingEditorCapabilities {
  bool deadZone = false;
  bool activationThreshold = false;
  bool releaseThreshold = false;
  bool inversion = false;
};

constexpr int inputBindingEditorControlCount(
    InputBindingEditorCapabilities capabilities) {
  return 1 + static_cast<int>(capabilities.deadZone) +
         static_cast<int>(capabilities.activationThreshold) +
         static_cast<int>(capabilities.releaseThreshold) +
         static_cast<int>(capabilities.inversion);
}

constexpr InputBindingEditorCapabilities
inputBindingEditorCapabilities(input::ControlKind kind) {
  switch (kind) {
  case input::ControlKind::Axis:
    return {.deadZone = true,
            .activationThreshold = true,
            .releaseThreshold = true,
            .inversion = true};
  case input::ControlKind::MidiNote:
    return {.activationThreshold = true};
  case input::ControlKind::MidiControl:
    return {.deadZone = true,
            .activationThreshold = true,
            .releaseThreshold = true};
  case input::ControlKind::Key:
  case input::ControlKind::Button:
  case input::ControlKind::Hat:
  case input::ControlKind::TouchRegion:
    return {};
  }
  return {};
}

constexpr InputSettingsLayout resolveInputSettingsLayout(int availableWidth,
                                                         bool compact) {
  InputSettingsLayout result;
  const int width = std::max(0, availableWidth);
  result.selectorGap = compact ? 8 : 12;
  result.actionGroupPadding = compact ? 12 : 16;
  result.stackSelectors = compact || width < 720;
  result.stackBindingEditor = compact || width < 900;
  result.selectorWidth =
      result.stackSelectors ? width
                            : std::max(0, (width - result.selectorGap * 2) / 3);
  result.bindingEditorWidth =
      std::max(0, width - result.actionGroupPadding * 2);
  return result;
}

constexpr int resolveInputBindingEditorControlWidth(
    InputSettingsLayout layout, InputBindingEditorCapabilities capabilities) {
  if (layout.stackBindingEditor) {
    return layout.bindingEditorWidth;
  }
  const int controlCount = inputBindingEditorControlCount(capabilities);
  return std::max(
      0, (layout.bindingEditorWidth - layout.selectorGap * (controlCount - 1)) /
             controlCount);
}

struct GyroscopeSettingsLayout {
  bool stackEditors = false;
  int editorWidth = 0;
};

constexpr GyroscopeSettingsLayout
resolveGyroscopeSettingsLayout(int availableWidth, bool compact) {
  constexpr int editorGap = 12;
  const int width = std::max(0, availableWidth);
  const bool stackEditors = compact || width < 640;
  return {.stackEditors = stackEditors,
          .editorWidth =
              stackEditors ? width : std::max(0, (width - editorGap) / 2)};
}

constexpr std::string_view deviceClassLabel(input::DeviceClass deviceClass) {
  switch (deviceClass) {
  case input::DeviceClass::Keyboard:
    return "Keyboard";
  case input::DeviceClass::GameController:
    return "Controller";
  case input::DeviceClass::Joystick:
    return "Joystick";
  case input::DeviceClass::Touch:
    return "Touch";
  case input::DeviceClass::Midi:
    return "MIDI";
  case input::DeviceClass::Gyroscope:
    return "Gyroscope";
  }
  return "Input";
}

inline std::string axisControlLabel(input::DeviceClass deviceClass, int index,
                                    input::ControlDirection direction) {
  std::string result =
      deviceClass == input::DeviceClass::Gyroscope && index == 0
          ? "Turntable"
          : "Axis " + std::to_string(index);
  if (direction == input::ControlDirection::Positive) {
    result += " +";
  } else if (direction == input::ControlDirection::Negative) {
    result += " -";
  }
  return result;
}

constexpr std::string_view
inputDeviceStatusLabel(input::InputDeviceStatus status) {
  switch (status) {
  case input::InputDeviceStatus::Ready:
    return "Ready";
  case input::InputDeviceStatus::Calibrating:
    return "Calibrating";
  case input::InputDeviceStatus::Disconnected:
    return "Disconnected";
  case input::InputDeviceStatus::Retrying:
    return "Retrying";
  }
  return "Disconnected";
}

inline std::optional<int> parseGyroscopeSettingInteger(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  int value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

} // namespace settings_scene
