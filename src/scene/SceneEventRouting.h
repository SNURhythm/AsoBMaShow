#pragma once

#include <SDL3/SDL.h>

#include "../input/SDLPointerEvent.h"

namespace scene_event_routing {
template <typename SceneType>
void dispatchApplicationBackgroundChange(SceneType *currentScene,
                                         bool background) {
  if (currentScene != nullptr) {
    currentScene->onApplicationBackgroundChanged(background);
  }
}

[[nodiscard]] inline constexpr bool shouldDispatchToScene(Uint32 eventType) {
  if (eventType >= SDL_EVENT_WINDOW_FIRST &&
      eventType <= SDL_EVENT_WINDOW_LAST) return true;
  switch (eventType) {
  case SDL_EVENT_QUIT:
  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP:
  case SDL_EVENT_TEXT_INPUT:
  case SDL_EVENT_TEXT_EDITING:
  case SDL_EVENT_MOUSE_MOTION:
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP:
  case SDL_EVENT_MOUSE_WHEEL:
  case SDL_EVENT_FINGER_DOWN:
  case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_CANCELED:
  case SDL_EVENT_FINGER_UP:
  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
  case SDL_EVENT_GAMEPAD_BUTTON_UP:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] inline constexpr bool
shouldDispatchToScene(const SDL_Event &event) {
  return shouldDispatchToScene(event.type) &&
         !sdl_pointer_event::isMouseSynthesizedTouch(event);
}
} // namespace scene_event_routing
