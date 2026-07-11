#pragma once

#include <compare>
#include <cstdint>
#include <string>

namespace input {

enum class DeviceClass {
  Keyboard,
  GameController,
  Joystick,
  Touch,
  Midi,
  Gyroscope
};
enum class InputDeviceStatus { Ready, Calibrating, Disconnected, Retrying };
enum class ControlKind {
  Key,
  Button,
  Axis,
  Hat,
  TouchRegion,
  MidiNote,
  MidiControl
};
enum class ControlDirection { Any, Negative, Positive, Up, Right, Down, Left };
enum class LogicalActionKind {
  Lane,
  ScratchClockwise,
  ScratchCounterClockwise,
  Start,
  Select,
  Pause,
  Retry,
  LaneCoverIncrease,
  LaneCoverDecrease
};

struct InputScope {
  int player = 1;
  int keyMode = 7;
  auto operator<=>(const InputScope &) const = default;
};

struct LogicalAction {
  LogicalActionKind kind = LogicalActionKind::Lane;
  int lane = 0;
  auto operator<=>(const LogicalAction &) const = default;
};

struct PhysicalControl {
  std::string deviceId;
  DeviceClass deviceClass = DeviceClass::Keyboard;
  ControlKind kind = ControlKind::Key;
  int index = 0;
  ControlDirection direction = ControlDirection::Any;
  auto operator<=>(const PhysicalControl &) const = default;
};

inline constexpr float kDefaultBindingActivationThreshold = 0.20F;
inline constexpr float kDefaultBindingReleaseThreshold = 0.10F;

struct InputBinding {
  std::string id;
  InputScope scope;
  LogicalAction action;
  PhysicalControl control;
  float deadZone = 0.0f;
  float activationThreshold = kDefaultBindingActivationThreshold;
  float releaseThreshold = kDefaultBindingReleaseThreshold;
  bool inverted = false;
};

struct PhysicalInputEvent {
  PhysicalControl control;
  double rawValue = 0.0;
  float normalizedValue = 0.0f;
  std::uint64_t timestampMicros = 0;
};

struct LogicalInputTransition {
  InputScope scope;
  LogicalAction action;
  bool pressed = false;
  float value = 0.0f;
};

struct InputDeviceSnapshot {
  std::string stableId;
  std::string displayName;
  DeviceClass deviceClass = DeviceClass::Keyboard;
  bool connected = false;
  InputDeviceStatus status = InputDeviceStatus::Ready;
  int buttons = 0;
  int axes = 0;
  int hats = 0;
};

} // namespace input
