#pragma once

#include <SDL2/SDL.h>

namespace scene_event_routing {
[[nodiscard]] inline constexpr bool shouldDispatchToScene(Uint32 eventType) {
  switch (eventType) {
  case SDL_QUIT:
  case SDL_WINDOWEVENT:
  case SDL_KEYDOWN:
  case SDL_KEYUP:
  case SDL_TEXTINPUT:
  case SDL_TEXTEDITING:
  case SDL_TEXTEDITING_EXT:
  case SDL_MOUSEMOTION:
  case SDL_MOUSEBUTTONDOWN:
  case SDL_MOUSEBUTTONUP:
  case SDL_MOUSEWHEEL:
  case SDL_FINGERDOWN:
  case SDL_FINGERMOTION:
  case SDL_FINGERUP:
  case SDL_CONTROLLERBUTTONDOWN:
  case SDL_CONTROLLERBUTTONUP:
    return true;
  default:
    return false;
  }
}
} // namespace scene_event_routing
