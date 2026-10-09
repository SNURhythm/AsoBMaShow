#pragma once

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <array>
#include <cstdint>

struct WindowsXInputSample {
  std::uint16_t buttons = 0;
  std::uint8_t leftTrigger = 0;
  std::uint8_t rightTrigger = 0;
  std::int16_t leftX = 0;
  std::int16_t leftY = 0;
  std::int16_t rightX = 0;
  std::int16_t rightY = 0;
};

struct WindowsGameControllerState {
  std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
  std::array<std::int16_t, SDL_GAMEPAD_AXIS_COUNT> axes{};
};

[[nodiscard]] SDL_Scancode windowsRealtimeSdlScancode(
    std::uint32_t virtualKey, std::uint32_t scanCode, bool extended) noexcept;

[[nodiscard]] WindowsGameControllerState
windowsRealtimeControllerState(const WindowsXInputSample &sample) noexcept;
