#pragma once

#include <SDL2/SDL.h>
#include <limits>

namespace sdl_pointer_event {

// Android touch pointer IDs are nonnegative; mouse emulation has its own owner.
inline constexpr SDL_FingerID kMouseFingerId =
    std::numeric_limits<SDL_FingerID>::min();

[[nodiscard]] inline constexpr bool
isTouchSynthesizedMouse(const SDL_Event &event) noexcept {
  if (event.type == SDL_MOUSEMOTION) return event.motion.which == SDL_TOUCH_MOUSEID;
  if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP)
    return event.button.which == SDL_TOUCH_MOUSEID;
  return false;
}

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
