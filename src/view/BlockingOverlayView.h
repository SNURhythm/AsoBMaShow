#pragma once

#include "View.h"

class BlockingOverlayView : public View {
public:
  using View::View;

private:
  bool handleEventsImpl(SDL_Event &event) override {
    switch (event.type) {
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEMOTION:
    case SDL_MOUSEWHEEL:
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    case SDL_TEXTINPUT:
    case SDL_TEXTEDITING:
    case SDL_TEXTEDITING_EXT:
      return false;
    default:
      return true;
    }
  }
};
