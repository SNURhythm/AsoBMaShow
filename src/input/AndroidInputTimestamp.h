#pragma once

#include <SDL3/SDL_events.h>
#include <limits>

namespace input::android {

// SDL's Android JNI entry points omit the event time. The Java adapter scopes
// the original uptime timestamp around each synchronous SDL dispatch. Keep it
// thread-local so audio/controller/lifecycle producers cannot inherit it.
inline thread_local Uint64 scopedInputTimestamp = 0;

inline void setInputTimestamp(Uint64 nativeTimestamp, Uint64 nativeNow,
                              Uint64 sdlNow) noexcept {
  if (nativeTimestamp == 0) {
    scopedInputTimestamp = 0;
  } else if (nativeTimestamp <= nativeNow) {
    const Uint64 age = nativeNow - nativeTimestamp;
    scopedInputTimestamp = age >= sdlNow ? 1 : sdlNow - age;
  } else {
    const Uint64 ahead = nativeTimestamp - nativeNow;
    const Uint64 maximum = std::numeric_limits<Uint64>::max();
    scopedInputTimestamp = ahead > maximum - sdlNow ? maximum : sdlNow + ahead;
  }
}

inline bool SDLCALL timestampFilter(void *, SDL_Event *event) {
  if (scopedInputTimestamp != 0) {
    switch (event->type) {
    case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_CANCELED:
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN: case SDL_EVENT_JOYSTICK_BUTTON_UP:
    case SDL_EVENT_JOYSTICK_AXIS_MOTION: case SDL_EVENT_JOYSTICK_HAT_MOTION:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      event->common.timestamp = scopedInputTimestamp;
      break;
    default:
      break;
    }
  }
  return true;
}

} // namespace input::android
