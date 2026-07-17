#include "WindowsRealtimeInputMapping.h"

#include <algorithm>
#include <array>
#include <limits>

namespace {

// Kept equivalent to SDL's Windows set-1 scan-code table so keys captured by
// the native realtime source resolve the same bindings as SDL events.
constexpr std::array<SDL_Scancode, 128> kWindowsScanCodes{
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_1,
    SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5,
    SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9,
    SDL_SCANCODE_0, SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS,
    SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB, SDL_SCANCODE_Q,
    SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_T,
    SDL_SCANCODE_Y, SDL_SCANCODE_U, SDL_SCANCODE_I, SDL_SCANCODE_O,
    SDL_SCANCODE_P, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET,
    SDL_SCANCODE_RETURN, SDL_SCANCODE_LCTRL, SDL_SCANCODE_A,
    SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_G,
    SDL_SCANCODE_H, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L,
    SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_GRAVE,
    SDL_SCANCODE_LSHIFT, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_Z,
    SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_B,
    SDL_SCANCODE_N, SDL_SCANCODE_M, SDL_SCANCODE_COMMA,
    SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_RSHIFT,
    SDL_SCANCODE_PRINTSCREEN, SDL_SCANCODE_LALT, SDL_SCANCODE_SPACE,
    SDL_SCANCODE_CAPSLOCK, SDL_SCANCODE_F1, SDL_SCANCODE_F2,
    SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5, SDL_SCANCODE_F6,
    SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_F10,
    SDL_SCANCODE_NUMLOCKCLEAR, SDL_SCANCODE_SCROLLLOCK, SDL_SCANCODE_HOME,
    SDL_SCANCODE_UP, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_KP_MINUS,
    SDL_SCANCODE_LEFT, SDL_SCANCODE_KP_5, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_END, SDL_SCANCODE_DOWN,
    SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_INSERT, SDL_SCANCODE_DELETE,
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_NONUSBACKSLASH, SDL_SCANCODE_F11, SDL_SCANCODE_F12,
    SDL_SCANCODE_PAUSE, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_LGUI,
    SDL_SCANCODE_RGUI, SDL_SCANCODE_APPLICATION, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_F13,
    SDL_SCANCODE_F14, SDL_SCANCODE_F15, SDL_SCANCODE_F16,
    SDL_SCANCODE_F17, SDL_SCANCODE_F18, SDL_SCANCODE_F19,
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_INTERNATIONAL2, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_INTERNATIONAL1,
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_INTERNATIONAL4, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_INTERNATIONAL5, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_INTERNATIONAL3, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_UNKNOWN};

SDL_Scancode virtualKeyScancode(std::uint32_t virtualKey) noexcept {
  switch (virtualKey) {
  case 0x08: return SDL_SCANCODE_BACKSPACE;
  case 0x14: return SDL_SCANCODE_CAPSLOCK;
  case 0x1F: return SDL_SCANCODE_MODE;
  case 0x29: return SDL_SCANCODE_SELECT;
  case 0x2B: return SDL_SCANCODE_EXECUTE;
  case 0x2F: return SDL_SCANCODE_HELP;
  case 0x13: return SDL_SCANCODE_PAUSE;
  case 0x90: return SDL_SCANCODE_NUMLOCKCLEAR;
  case 0x7C: return SDL_SCANCODE_F13;
  case 0x7D: return SDL_SCANCODE_F14;
  case 0x7E: return SDL_SCANCODE_F15;
  case 0x7F: return SDL_SCANCODE_F16;
  case 0x80: return SDL_SCANCODE_F17;
  case 0x81: return SDL_SCANCODE_F18;
  case 0x82: return SDL_SCANCODE_F19;
  case 0x83: return SDL_SCANCODE_F20;
  case 0x84: return SDL_SCANCODE_F21;
  case 0x85: return SDL_SCANCODE_F22;
  case 0x86: return SDL_SCANCODE_F23;
  case 0x87: return SDL_SCANCODE_F24;
  case 0x92: return SDL_SCANCODE_KP_EQUALS;
  case 0xA6: return SDL_SCANCODE_AC_BACK;
  case 0xA7: return SDL_SCANCODE_AC_FORWARD;
  case 0xA8: return SDL_SCANCODE_AC_REFRESH;
  case 0xA9: return SDL_SCANCODE_AC_STOP;
  case 0xAA: return SDL_SCANCODE_AC_SEARCH;
  case 0xAB: return SDL_SCANCODE_AC_BOOKMARKS;
  case 0xAC: return SDL_SCANCODE_AC_HOME;
  case 0xAD: return SDL_SCANCODE_MUTE;
  case 0xAE: return SDL_SCANCODE_VOLUMEDOWN;
  case 0xAF: return SDL_SCANCODE_VOLUMEUP;
  case 0xB0: return SDL_SCANCODE_AUDIONEXT;
  case 0xB1: return SDL_SCANCODE_AUDIOPREV;
  case 0xB2: return SDL_SCANCODE_AUDIOSTOP;
  case 0xB3: return SDL_SCANCODE_AUDIOPLAY;
  case 0xB4: return SDL_SCANCODE_MAIL;
  case 0xB5: return SDL_SCANCODE_MEDIASELECT;
  case 0xB6: return SDL_SCANCODE_APP1;
  case 0xB7: return SDL_SCANCODE_APP2;
  case 0xE2: return SDL_SCANCODE_NONUSBACKSLASH;
  case 0xF6: return SDL_SCANCODE_SYSREQ;
  case 0xF7: return SDL_SCANCODE_CRSEL;
  case 0xF8: return SDL_SCANCODE_EXSEL;
  case 0xFE: return SDL_SCANCODE_CLEAR;
  default: return SDL_SCANCODE_UNKNOWN;
  }
}

std::int16_t invertYAxis(std::int16_t value) noexcept {
  if (value == std::numeric_limits<std::int16_t>::min()) {
    return std::numeric_limits<std::int16_t>::max();
  }
  return static_cast<std::int16_t>(-value);
}

std::int16_t triggerAxis(std::uint8_t value) noexcept {
  return static_cast<std::int16_t>(
      static_cast<std::uint32_t>(value) * 32767U / 255U);
}

} // namespace

SDL_Scancode windowsRealtimeSdlScancode(std::uint32_t virtualKey,
                                        std::uint32_t scanCode,
                                        bool extended) noexcept {
  SDL_Scancode result = virtualKeyScancode(virtualKey);
  if (result == SDL_SCANCODE_UNKNOWN && scanCode < kWindowsScanCodes.size()) {
    result = kWindowsScanCodes[scanCode];
    if (extended) {
      switch (result) {
      case SDL_SCANCODE_RETURN: result = SDL_SCANCODE_KP_ENTER; break;
      case SDL_SCANCODE_LALT: result = SDL_SCANCODE_RALT; break;
      case SDL_SCANCODE_LCTRL: result = SDL_SCANCODE_RCTRL; break;
      case SDL_SCANCODE_SLASH: result = SDL_SCANCODE_KP_DIVIDE; break;
      case SDL_SCANCODE_CAPSLOCK: result = SDL_SCANCODE_KP_PLUS; break;
      default: break;
      }
    } else {
      switch (result) {
      case SDL_SCANCODE_HOME: result = SDL_SCANCODE_KP_7; break;
      case SDL_SCANCODE_UP: result = SDL_SCANCODE_KP_8; break;
      case SDL_SCANCODE_PAGEUP: result = SDL_SCANCODE_KP_9; break;
      case SDL_SCANCODE_LEFT: result = SDL_SCANCODE_KP_4; break;
      case SDL_SCANCODE_RIGHT: result = SDL_SCANCODE_KP_6; break;
      case SDL_SCANCODE_END: result = SDL_SCANCODE_KP_1; break;
      case SDL_SCANCODE_DOWN: result = SDL_SCANCODE_KP_2; break;
      case SDL_SCANCODE_PAGEDOWN: result = SDL_SCANCODE_KP_3; break;
      case SDL_SCANCODE_INSERT: result = SDL_SCANCODE_KP_0; break;
      case SDL_SCANCODE_DELETE: result = SDL_SCANCODE_KP_PERIOD; break;
      case SDL_SCANCODE_PRINTSCREEN: result = SDL_SCANCODE_KP_MULTIPLY; break;
      default: break;
      }
    }
  }

  if (result == SDL_SCANCODE_UNKNOWN && scanCode == 0) {
    switch (virtualKey) {
    case 0x25: return SDL_SCANCODE_LEFT;
    case 0x26: return SDL_SCANCODE_UP;
    case 0x27: return SDL_SCANCODE_RIGHT;
    case 0x28: return SDL_SCANCODE_DOWN;
    case 0x11: return SDL_SCANCODE_LCTRL;
    case 0x56: return SDL_SCANCODE_V;
    default: break;
    }
  }
  return result;
}

WindowsGameControllerState
windowsRealtimeControllerState(const WindowsXInputSample &sample) noexcept {
  WindowsGameControllerState result;
  constexpr std::array buttonMasks{
      std::pair{SDL_CONTROLLER_BUTTON_A, std::uint16_t{0x1000}},
      std::pair{SDL_CONTROLLER_BUTTON_B, std::uint16_t{0x2000}},
      std::pair{SDL_CONTROLLER_BUTTON_X, std::uint16_t{0x4000}},
      std::pair{SDL_CONTROLLER_BUTTON_Y, std::uint16_t{0x8000}},
      std::pair{SDL_CONTROLLER_BUTTON_BACK, std::uint16_t{0x0020}},
      std::pair{SDL_CONTROLLER_BUTTON_GUIDE, std::uint16_t{0x0400}},
      std::pair{SDL_CONTROLLER_BUTTON_START, std::uint16_t{0x0010}},
      std::pair{SDL_CONTROLLER_BUTTON_LEFTSTICK, std::uint16_t{0x0040}},
      std::pair{SDL_CONTROLLER_BUTTON_RIGHTSTICK, std::uint16_t{0x0080}},
      std::pair{SDL_CONTROLLER_BUTTON_LEFTSHOULDER, std::uint16_t{0x0100}},
      std::pair{SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, std::uint16_t{0x0200}},
      std::pair{SDL_CONTROLLER_BUTTON_DPAD_UP, std::uint16_t{0x0001}},
      std::pair{SDL_CONTROLLER_BUTTON_DPAD_DOWN, std::uint16_t{0x0002}},
      std::pair{SDL_CONTROLLER_BUTTON_DPAD_LEFT, std::uint16_t{0x0004}},
      std::pair{SDL_CONTROLLER_BUTTON_DPAD_RIGHT, std::uint16_t{0x0008}}};
  for (const auto &[button, mask] : buttonMasks) {
    result.buttons[static_cast<std::size_t>(button)] =
        (sample.buttons & mask) != 0;
  }
  result.axes[SDL_CONTROLLER_AXIS_LEFTX] = sample.leftX;
  result.axes[SDL_CONTROLLER_AXIS_LEFTY] = invertYAxis(sample.leftY);
  result.axes[SDL_CONTROLLER_AXIS_RIGHTX] = sample.rightX;
  result.axes[SDL_CONTROLLER_AXIS_RIGHTY] = invertYAxis(sample.rightY);
  result.axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] =
      triggerAxis(sample.leftTrigger);
  result.axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] =
      triggerAxis(sample.rightTrigger);
  return result;
}
