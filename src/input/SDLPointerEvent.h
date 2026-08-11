#pragma once

#include <SDL2/SDL.h>

namespace sdl_pointer_event {

[[nodiscard]] inline constexpr bool
isMouseSynthesizedTouch(const SDL_Event &event) noexcept {
  switch (event.type) {
  case SDL_FINGERDOWN:
  case SDL_FINGERMOTION:
  case SDL_FINGERUP:
    return event.tfinger.touchId == SDL_MOUSE_TOUCHID;
  default:
    return false;
  }
}

} // namespace sdl_pointer_event
