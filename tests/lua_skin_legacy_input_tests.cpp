#include "input/InputDeviceRegistry.h"
#include "skin/beatoraja/LuaSkinLegacyInputHost.h"
#include <SDL3/SDL_events.h>
#include <iostream>
#include <limits>
#include <type_traits>

namespace {
int failures = 0;
void expect(bool value, const char *message) {
  if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
void key(InputDeviceRegistry &registry, SDL_EventType type, SDL_Scancode code,
         bool repeat = false) {
  SDL_Event e{}; e.type = type; e.key.scancode = code; e.key.repeat = repeat;
  registry.handleSdlEventAndDispatch(e);
}
void testKeyNames() {
  using Host = skin::LuaSkinLegacyInputHost;
  expect(Host::keyConstant("UP") == 19 && Host::keyConstant("Enter") == 66 &&
             Host::keyConstant("dpad_up") == 19 && Host::keyConstant("BACKSPACE") == 67 &&
             Host::keyConstant("NUM_0") == 7 && Host::keyConstant("CONTROL_LEFT") == 129 &&
             Host::keyConstant("BUTTON_CIRCLE") == 255 && Host::keyConstant("ANY_KEY") == -1,
         "Java names and aliases resolve case insensitively, including ANY_KEY");
  expect(Host::keyConstant("L-Ctrl") == 129 && Host::keyConstant("Page Up") == 92 &&
             !Host::keyConstant("page up") && !Host::keyConstant("not_a_key") &&
             !Host::keyConstant(""), "display fallback is case sensitive; unknown is absent");
  expect(Host::keyCode("UP") == -1 && Host::keyCode("Enter") == 66 &&
             Host::keyCode("ANY_KEY") == -1, "main_state display-name contract stays unchanged");
}
void testKeyFrames() {
  InputDeviceRegistry registry(std::vector<InputDeviceRegistry::BackendFactory>{});
  skin::LuaSkinLegacyInputHost host;
  key(registry, SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN);
  key(registry, SDL_EVENT_KEY_UP, SDL_SCANCODE_RETURN);
  registry.pump(); registry.pump(); // Frame pacing dispatch must not consume input.
  registry.advanceLegacyInputFrame(640, 360);
  host.publish(registry.legacyInputGeneration(1280, 720));
  expect(host.isKeyJustPressed(66) && !host.isKeyPressed(66) && host.hasCurrentPressEdge() &&
             registry.hasCurrentLegacyPressEdge(),
         "a rapid Enter tap survives release and authorizes this frame");
  host.publish(registry.legacyInputGeneration(1280, 720));
  expect(host.isKeyJustPressed(66), "multiple captures do not consume the frame edge");
  registry.advanceLegacyInputFrame(640, 360);
  host.publish(registry.legacyInputGeneration(1280, 720));
  expect(!host.isKeyJustPressed(66) && !host.hasCurrentPressEdge() &&
             !registry.hasCurrentLegacyPressEdge(), "edge expires on an idle frame");
  key(registry, SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE);
  registry.advanceLegacyInputFrame(640, 360);
  host.publish(registry.legacyInputGeneration(1280, 720));
  expect(host.isKeyPressed(62) && host.hasCurrentPressEdge(), "Space down is a browser gesture");
  key(registry, SDL_EVENT_KEY_DOWN, SDL_SCANCODE_SPACE, true);
  registry.advanceLegacyInputFrame(640, 360);
  host.publish(registry.legacyInputGeneration(1280, 720));
  expect(host.isKeyPressed(62) && !host.isKeyJustPressed(62) && !host.hasCurrentPressEdge(),
         "held and repeated keys do not renew browser authorization");
  expect(!host.isKeyJustPressed(-1) && !host.isKeyJustPressed(256), "just-key facade rejects out of range");
  key(registry, SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A);
  registry.advanceLegacyInputFrame(640, 360);
  host.publish(registry.legacyInputGeneration(1280, 720));
  expect(host.isKeyJustPressed(29) && !host.hasCurrentPressEdge(), "other keys are not browser gestures");
  key(registry, SDL_EVENT_KEY_DOWN, SDL_SCANCODE_RETURN);
  SDL_Event focus{}; focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  registry.handleSdlEvent(focus);
  host.publish(registry.legacyInputGeneration(1280, 720));
  expect(!host.isKeyPressed(-1) && !host.isKeyJustPressed(29) &&
             !registry.hasCurrentLegacyPressEdge(), "focus loss clears published held and edge state");
  registry.advanceLegacyInputFrame(640, 360);
  host.publish(registry.legacyInputGeneration(1280, 720));
  expect(!host.hasCurrentPressEdge(), "focus loss also clears pending gestures");
}
void testPointerFrames() {
  InputDeviceRegistry registry(std::vector<InputDeviceRegistry::BackendFactory>{});
  skin::LuaSkinLegacyInputHost host;
  SDL_Event e{}; e.type = SDL_EVENT_MOUSE_MOTION;
  e.motion.x = 120; e.motion.y = 60; e.motion.xrel = 5; e.motion.yrel = -3;
  registry.handleSdlEvent(e);
  e.motion.x = 122; e.motion.y = 61; e.motion.xrel = 2; e.motion.yrel = 1;
  registry.handleSdlEvent(e);
  registry.advanceLegacyInputFrame(640, 360);
  host.publish(registry.legacyInputGeneration(1280, 1080));
  expect(host.getX() == 244 && host.getY() == 183 && host.getDeltaX() == 14 && host.getDeltaY() == -6,
         "mouse position and accumulated delta scale from logical to drawable top-left coordinates");
  registry.advanceLegacyInputFrame(640, 360);
  host.publish(registry.legacyInputGeneration(640, 360));
  expect(host.getX() == 122 && host.getY() == 61 && host.getDeltaX() == 0 && host.getDeltaY() == 0,
         "stationary pointer survives dimension changes while motion expires");
  const Uint8 buttons[] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE, SDL_BUTTON_X1, SDL_BUTTON_X2};
  for (int i = 0; i != 5; ++i) {
    e = {}; e.type = SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.button = buttons[i];
    e.button.x = 130; e.button.y = 65; registry.handleSdlEvent(e);
    registry.advanceLegacyInputFrame(640, 360);
    host.publish(registry.legacyInputGeneration(1280, 720));
    expect(host.isButtonPressed(i) && host.justTouched(), "SDL mouse order maps to each Gdx button");
    e.type = SDL_EVENT_MOUSE_BUTTON_UP; registry.handleSdlEvent(e);
  }
  registry.advanceLegacyInputFrame(640, 360);
  host.publish(registry.legacyInputGeneration(1280, 720));
  expect(!host.isButtonPressed(-1) && !host.isButtonPressed(5) && !host.isButtonPressed(4) && !host.justTouched(),
         "released and invalid buttons are false; press edge expires");
  e = {}; e.type = SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.button = SDL_BUTTON_LEFT;
  registry.handleSdlEvent(e); e.type = SDL_EVENT_MOUSE_BUTTON_UP; registry.handleSdlEvent(e);
  registry.advanceLegacyInputFrame(640, 360); host.publish(registry.legacyInputGeneration(1280, 720));
  expect(host.justTouched() && host.hasCurrentPressEdge() && !host.isButtonPressed(0), "rapid mouse tap retains gesture edge");
  e = {}; e.type = SDL_EVENT_FINGER_DOWN; e.tfinger.touchID = 1; e.tfinger.fingerID = 10;
  e.tfinger.x = .25F; e.tfinger.y = .5F; registry.handleSdlEvent(e);
  e.type = SDL_EVENT_FINGER_MOTION; e.tfinger.x = .375F; e.tfinger.dx = .125F;
  e.tfinger.y = .25F; e.tfinger.dy = -.25F; registry.handleSdlEvent(e);
  registry.advanceLegacyInputFrame(640, 360); host.publish(registry.legacyInputGeneration(1280, 720));
  expect(host.getX() == 480 && host.getY() == 180 && host.getDeltaX() == 160 && host.getDeltaY() == -180 &&
             host.isButtonPressed(0) && host.justTouched(), "native normalized touch supplies scaled pointer and left-button state");
  e.type = SDL_EVENT_FINGER_CANCELED; registry.handleSdlEvent(e);
  registry.advanceLegacyInputFrame(640, 360); host.publish(registry.legacyInputGeneration(-1, -2));
  expect(!host.isButtonPressed(0) && !host.justTouched() && host.drawableWidth() == 0 && host.getX() == 0,
         "touch cancellation releases button; invalid dimensions normalize safely");
  e = {}; e.type = SDL_EVENT_MOUSE_BUTTON_DOWN; e.button.which = SDL_TOUCH_MOUSEID; e.button.button = SDL_BUTTON_LEFT;
  registry.handleSdlEvent(e);
  e = {}; e.type = SDL_EVENT_FINGER_DOWN; e.tfinger.touchID = SDL_MOUSE_TOUCHID;
  registry.handleSdlEvent(e);
  registry.advanceLegacyInputFrame(640, 360); host.publish(registry.legacyInputGeneration(1280, 720));
  expect(!host.justTouched() && !host.isButtonPressed(0), "SDL synthetic touch/mouse duplicates cannot create gestures");
}
void testTouchOwnershipAndCancellation() {
  InputDeviceRegistry registry(std::vector<InputDeviceRegistry::BackendFactory>{});
  skin::LuaSkinLegacyInputHost host;
  SDL_Event e{};
  e.type = SDL_EVENT_FINGER_DOWN;
  e.tfinger.touchID = 2;
  e.tfinger.fingerID = 1;
  e.tfinger.x = .25F;
  e.tfinger.y = .5F;
  registry.handleSdlEvent(e);
  e.tfinger.fingerID = 2;
  e.tfinger.x = .75F;
  registry.handleSdlEvent(e);
  registry.advanceLegacyInputFrame(100, 100);
  host.publish(registry.legacyInputGeneration(100, 100));
  expect(host.getX() == 25 && host.getY() == 50 && host.isButtonPressed(0),
         "secondary touch does not move Gdx pointer zero");
  e.type = SDL_EVENT_FINGER_UP;
  registry.handleSdlEvent(e);
  registry.advanceLegacyInputFrame(100, 100);
  host.publish(registry.legacyInputGeneration(100, 100));
  expect(host.isButtonPressed(0) && !host.justTouched(),
         "releasing secondary touch leaves primary pressed without renewing its edge");
  e.type = SDL_EVENT_FINGER_MOTION;
  e.tfinger.fingerID = 1;
  e.tfinger.x = std::numeric_limits<float>::quiet_NaN();
  registry.handleSdlEvent(e);
  registry.advanceLegacyInputFrame(100, 100);
  host.publish(registry.legacyInputGeneration(100, 100));
  expect(host.getX() == 25 && host.getDeltaX() == 0,
         "nonfinite native touch coordinates cannot poison position or delta");
  e.type = SDL_EVENT_FINGER_CANCELED;
  registry.handleSdlEvent(e);
  registry.advanceLegacyInputFrame(100, 100);
  host.publish(registry.legacyInputGeneration(100, 100));
  expect(!host.isButtonPressed(0) && !host.hasCurrentPressEdge(),
         "even a cancellation without valid coordinates releases the active finger");
  e = {};
  e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  e.button.button = SDL_BUTTON_LEFT;
  e.button.x = 30;
  e.button.y = 40;
  registry.handleSdlEvent(e);
  registry.advanceLegacyInputFrame(100, 100);
  e = {};
  e.type = SDL_EVENT_WINDOW_FOCUS_LOST;
  registry.handleSdlEvent(e);
  host.publish(registry.legacyInputGeneration(100, 100));
  expect(!host.isButtonPressed(0) && !host.justTouched() && host.getX() == 30,
         "focus loss clears mouse buttons and edges while preserving stationary position");
  registry.advanceLegacyInputFrame(0, -1);
  host.publish(registry.legacyInputGeneration(100, 100));
  expect(host.getX() == 0 && host.getY() == 0,
         "absent logical dimensions never divide by zero");
}
void testValueSnapshotNormalization() {
  skin::LuaSkinLegacyInputSnapshot snapshot;
  snapshot.drawableWidth = -1;
  snapshot.drawableHeight = 720;
  snapshot.justPressedKeys = {-1, 66, 256};
  snapshot.pressedMouseButtons.set(4);
  snapshot.pointerX = 42;
  snapshot.pointerY = 43;
  snapshot.deltaX = -2;
  snapshot.deltaY = 3;
  snapshot.justTouched = true;
  skin::LuaSkinLegacyInputHost host(std::move(snapshot));
  expect(host.drawableWidth() == 0 && host.drawableHeight() == 720 &&
             host.isKeyJustPressed(66) && !host.isKeyJustPressed(-1) &&
             !host.isKeyJustPressed(256) && !host.isKeyPressed(66) &&
             host.isButtonPressed(4) && host.hasCurrentPressEdge() &&
             host.getX() == 42 && host.getY() == 43 &&
             host.getDeltaX() == -2 && host.getDeltaY() == 3,
         "value-only fixtures normalize dimensions and invalid keys without inventing held state");
  (void)host.publish(skin::LuaSkinLegacyInputSnapshot{});
  expect(!host.hasCurrentPressEdge() && !host.isButtonPressed(4) && host.getX() == 0,
         "replacement snapshot expires every prior edge and pointer value");
}

}
int main() {
  static_assert(std::is_trivially_copyable_v<input::LegacyInputGeneration>);
  testKeyNames(); testKeyFrames(); testPointerFrames();
  testTouchOwnershipAndCancellation(); testValueSnapshotNormalization();
  if (failures) return 1;
  std::cout << "legacy input tests passed\n";
}
