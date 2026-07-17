#pragma once

#include <SDL2/SDL_gamecontroller.h>
#include <SDL2/SDL_scancode.h>

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
  std::array<bool, SDL_CONTROLLER_BUTTON_MAX> buttons{};
  std::array<std::int16_t, SDL_CONTROLLER_AXIS_MAX> axes{};
};

[[nodiscard]] SDL_Scancode windowsRealtimeSdlScancode(
    std::uint32_t virtualKey, std::uint32_t scanCode, bool extended) noexcept;

[[nodiscard]] WindowsGameControllerState
windowsRealtimeControllerState(const WindowsXInputSample &sample) noexcept;
