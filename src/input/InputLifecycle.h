#pragma once

#include <SDL2/SDL_events.h>

namespace input {

[[nodiscard]] inline bool
isBackgroundLifecycleEvent(const SDL_Event &event) noexcept {
  return event.type == SDL_APP_WILLENTERBACKGROUND ||
         event.type == SDL_APP_DIDENTERBACKGROUND ||
         (event.type == SDL_WINDOWEVENT &&
          (event.window.event == SDL_WINDOWEVENT_MINIMIZED ||
           event.window.event == SDL_WINDOWEVENT_HIDDEN ||
           event.window.event == SDL_WINDOWEVENT_FOCUS_LOST));
}

[[nodiscard]] inline bool
isForegroundLifecycleEvent(const SDL_Event &event) noexcept {
  return event.type == SDL_APP_WILLENTERFOREGROUND ||
         event.type == SDL_APP_DIDENTERFOREGROUND ||
         (event.type == SDL_WINDOWEVENT &&
          (event.window.event == SDL_WINDOWEVENT_RESTORED ||
           event.window.event == SDL_WINDOWEVENT_SHOWN ||
           event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED));
}

} // namespace input
