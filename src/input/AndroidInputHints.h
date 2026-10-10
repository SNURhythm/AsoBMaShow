#pragma once

#include <SDL3/SDL_hints.h>

namespace input::android {

inline void configurePointerHints() {
  // Native touch and mouse are routed independently. SDL otherwise queues a
  // synthetic counterpart before the original reaches the realtime watch.
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
  SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
}

} // namespace input::android
