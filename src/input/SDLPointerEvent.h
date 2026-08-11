#pragma once

#include <SDL2/SDL.h>

namespace sdl_pointer_event {

[[nodiscard]] inline constexpr float
verticalWheelScrollDelta(const SDL_MouseWheelEvent &event,
                         float uiUnitsPerWheelStep) noexcept {
  // SDL's iPad mouse backend changes the sign of preciseY when the user
  // enables Natural Scrolling and marks that event FLIPPED. Preserve that
  // signed delta instead of normalizing it back to a platform-neutral wheel.
  const float wheelDelta =
      event.preciseY != 0.0F ? event.preciseY : static_cast<float>(event.y);
  return -wheelDelta * uiUnitsPerWheelStep;
}

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
