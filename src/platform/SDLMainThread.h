#pragma once

#include "IOSApplicationRuntime.h"
#include "SDLApplicationRuntime.h"
#include <SDL3/SDL.h>
#include <string>

namespace platform {
#if TARGET_OS_IPHONE
inline void setApplicationEventDiscardHandler(std::function<void(const SDL_Event &)> handler) {
  SetIOSApplicationEventDiscardHandler(std::move(handler));
}
#endif
// SDL errors are thread-local. Preserve a failed operation's diagnostic on its
// caller, rather than reporting an unrelated error from the worker's TLS.
template <auto Function, typename... Args> auto sdlMain(Args &&...args) {
  using Result = std::invoke_result_t<decltype(Function), Args...>;
  std::string error;
  if constexpr (std::is_void_v<Result>) {
    onMain([&] { Function(std::forward<Args>(args)...); });
  } else {
    auto result = onMain([&] {
      auto value = Function(std::forward<Args>(args)...);
      if constexpr (std::is_same_v<Result, bool> || std::is_pointer_v<Result>) {
        if (!value) error = SDL_GetError();
      }
      return value;
    });
    if (!error.empty()) SDL_SetError("%s", error.c_str());
    return result;
  }
}

inline bool windowSize(SDL_Window *window, int *width, int *height) {
#if TARGET_OS_IPHONE
  const auto state = GetIOSWindowSnapshot(window);
#else
  const auto state = getWindowSnapshot(window);
#endif
  if (state) {
    if (width) *width = state->width;
    if (height) *height = state->height;
    return true;
  }
  return sdlMain<SDL_GetWindowSize>(window, width, height);
}

inline bool windowSizeInPixels(SDL_Window *window, int *width, int *height) {
#if TARGET_OS_IPHONE
  const auto state = GetIOSWindowSnapshot(window);
#else
  const auto state = getWindowSnapshot(window);
#endif
  if (state) {
    if (width) *width = state->pixelWidth;
    if (height) *height = state->pixelHeight;
    return true;
  }
  return sdlMain<SDL_GetWindowSizeInPixels>(window, width, height);
}

inline SDL_WindowID windowID(SDL_Window *window) {
#if TARGET_OS_IPHONE
  const auto state = GetIOSWindowSnapshot(window);
#else
  const auto state = getWindowSnapshot(window);
#endif
  if (state) return state->id;
  return sdlMain<SDL_GetWindowID>(window);
}

inline void startFocusedTextInput() {
  onMain([] { SDL_StartTextInput(SDL_GetKeyboardFocus()); });
}
inline void stopFocusedTextInput() {
  onMain([] { SDL_StopTextInput(SDL_GetKeyboardFocus()); });
}
inline void clearFocusedComposition() {
  onMain([] { SDL_ClearComposition(SDL_GetKeyboardFocus()); });
}
}
