#include "input/WindowsRealtimeInputMapping.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testKeyboardMappingMatchesSdlWindowsRules() {
  expect(windowsRealtimeSdlScancode(0x41, 0x1E, false) == SDL_SCANCODE_A,
         "set-1 letter scan codes map to SDL scancodes");
  expect(windowsRealtimeSdlScancode(0x0D, 0x1C, true) ==
             SDL_SCANCODE_KP_ENTER,
         "extended return maps to keypad enter");
  expect(windowsRealtimeSdlScancode(0x11, 0x1D, true) ==
             SDL_SCANCODE_RCTRL,
         "extended control maps to right control");
  expect(windowsRealtimeSdlScancode(0x24, 0x47, false) == SDL_SCANCODE_KP_7,
         "non-extended navigation scan codes map to keypad keys");
  expect(windowsRealtimeSdlScancode(0x7F, 0, false) == SDL_SCANCODE_F16,
         "virtual-key-only extended function keys remain bindable");
}

void testXInputMappingMatchesSdlControllerLayout() {
  const auto state = windowsRealtimeControllerState(
      {.buttons = static_cast<std::uint16_t>(0x1000 | 0x0001 | 0x0200),
       .leftTrigger = 255,
       .rightTrigger = 0,
       .leftX = -1234,
       .leftY = 2000,
       .rightX = 3000,
       .rightY = std::numeric_limits<std::int16_t>::min()});
  expect(state.buttons[SDL_CONTROLLER_BUTTON_A],
         "XInput A maps to SDL controller A");
  expect(state.buttons[SDL_CONTROLLER_BUTTON_DPAD_UP],
         "XInput dpad up maps to SDL controller dpad up");
  expect(state.buttons[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER],
         "XInput shoulder maps to SDL controller shoulder");
  expect(!state.buttons[SDL_CONTROLLER_BUTTON_B],
         "inactive XInput buttons stay inactive");
  expect(state.axes[SDL_CONTROLLER_AXIS_LEFTX] == -1234,
         "horizontal stick axes retain XInput polarity");
  expect(state.axes[SDL_CONTROLLER_AXIS_LEFTY] == -2000,
         "vertical stick axes use SDL polarity");
  expect(state.axes[SDL_CONTROLLER_AXIS_RIGHTY] == 32767,
         "minimum XInput Y saturates safely when inverted");
  expect(state.axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] == 32767 &&
             state.axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] == 0,
         "XInput triggers use SDL's zero-to-positive axis range");
}
} // namespace

int main() {
  testKeyboardMappingMatchesSdlWindowsRules();
  testXInputMappingMatchesSdlControllerLayout();
  return failures == 0 ? 0 : 1;
}
