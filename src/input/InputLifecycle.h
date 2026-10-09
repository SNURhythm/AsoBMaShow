#pragma once

#include <SDL3/SDL_events.h>

namespace input {

[[nodiscard]] inline bool
isBackgroundLifecycleEvent(const SDL_Event &event) noexcept {
  return event.type == SDL_EVENT_WILL_ENTER_BACKGROUND ||
         event.type == SDL_EVENT_DID_ENTER_BACKGROUND ||
         ((event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) &&
          (event.type == SDL_EVENT_WINDOW_MINIMIZED ||
           event.type == SDL_EVENT_WINDOW_HIDDEN ||
           event.type == SDL_EVENT_WINDOW_FOCUS_LOST));
}

[[nodiscard]] inline bool
isForegroundLifecycleEvent(const SDL_Event &event) noexcept {
  return event.type == SDL_EVENT_WILL_ENTER_FOREGROUND ||
         event.type == SDL_EVENT_DID_ENTER_FOREGROUND ||
         ((event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST) &&
          (event.type == SDL_EVENT_WINDOW_RESTORED ||
           event.type == SDL_EVENT_WINDOW_SHOWN ||
           event.type == SDL_EVENT_WINDOW_FOCUS_GAINED));
}

} // namespace input
