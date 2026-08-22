#pragma once

#include <algorithm>
#include <array>
#include <bitset>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace input {

inline constexpr std::size_t kLegacyInputMaximumControllers = 16;
inline constexpr std::size_t kLegacyInputMaximumButtons = 256;
inline constexpr std::size_t kLegacyInputMaximumGdxKeyCode = 255;
inline constexpr std::size_t kLegacyInputControllerNameBytes = 127;

struct LegacyControllerState {
  std::array<char, kLegacyInputControllerNameBytes + 1> nameBytes{};
  std::uint16_t nameLength = 0;
  std::bitset<kLegacyInputMaximumButtons> pressedButtons;

  void setName(std::string_view value) noexcept {
    nameLength = static_cast<std::uint16_t>(
        std::min(value.size(), kLegacyInputControllerNameBytes));
    if (nameLength != 0) {
      std::copy_n(value.data(), nameLength, nameBytes.data());
    }
    nameBytes[nameLength] = '\0';
  }

  [[nodiscard]] std::string_view name() const noexcept {
    return {nameBytes.data(), nameLength};
  }
};

// Bounded, allocation-free publication consumed by the finite LibGDX facade.
// Backends update this generation from SDL lifecycle/input events; a gameplay
// frame only copies the fixed value.
struct LegacyInputGeneration {
  std::uint64_t sequence = 0;
  int drawableWidth = 0;
  int drawableHeight = 0;
  bool anyKeyPressed = false;
  std::bitset<kLegacyInputMaximumGdxKeyCode + 1> pressedGdxKeys;
  std::array<LegacyControllerState, kLegacyInputMaximumControllers>
      controllers{};
  std::size_t controllerCount = 0;
};

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
enum class InputTimestampDomain { SteadyClock, SdlMilliseconds };
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
  InputTimestampDomain timestampDomain = InputTimestampDomain::SteadyClock;
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
