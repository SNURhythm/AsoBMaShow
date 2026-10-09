#pragma once

#include <SDL3/SDL.h>
#include <limits>

namespace sdl_pointer_event {

// Android touch pointer IDs are nonnegative; mouse emulation has its own owner.
inline constexpr SDL_FingerID kMouseFingerId =
    (SDL_FingerID{1} << 63);

[[nodiscard]] inline constexpr bool
isTouchSynthesizedMouse(const SDL_Event &event) noexcept {
  if (event.type == SDL_EVENT_MOUSE_MOTION) return event.motion.which == SDL_TOUCH_MOUSEID;
  if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP)
    return event.button.which == SDL_TOUCH_MOUSEID;
  return false;
}

[[nodiscard]] inline constexpr float
verticalWheelScrollDelta(const SDL_MouseWheelEvent &event,
                         float uiUnitsPerWheelStep) noexcept {
  // SDL's iPad mouse backend changes the sign of wheel motion when the user
  // enables Natural Scrolling and marks that event FLIPPED. Preserve that
  // signed delta instead of normalizing it back to a platform-neutral wheel.
  const float wheelDelta =
      event.y;
  return -wheelDelta * uiUnitsPerWheelStep;
}

[[nodiscard]] inline constexpr bool
isMouseSynthesizedTouch(const SDL_Event &event) noexcept {
  switch (event.type) {
  case SDL_EVENT_FINGER_DOWN:
  case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_UP:
  case SDL_EVENT_FINGER_CANCELED:
    return event.tfinger.touchID == SDL_MOUSE_TOUCHID;
  default:
    return false;
  }
}

} // namespace sdl_pointer_event
