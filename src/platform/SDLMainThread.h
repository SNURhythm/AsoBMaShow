#pragma once

#include "IOSApplicationRuntime.h"
#include <SDL3/SDL.h>
#include <string>

namespace platform {
// SDL errors are thread-local. Preserve a failed operation's diagnostic on its
// caller, rather than reporting an unrelated error from the worker's TLS.
template <auto Function, typename... Args> auto sdlMain(Args &&...args) {
#if TARGET_OS_IPHONE
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
#else
  return Function(std::forward<Args>(args)...);
#endif
}

inline bool windowSize(SDL_Window *window, int *width, int *height) {
#if TARGET_OS_IPHONE
  if (const auto state = GetIOSWindowSnapshot(window)) {
    if (width) *width = state->width;
    if (height) *height = state->height;
    return true;
  }
#endif
  return sdlMain<SDL_GetWindowSize>(window, width, height);
}

inline bool windowSizeInPixels(SDL_Window *window, int *width, int *height) {
#if TARGET_OS_IPHONE
  if (const auto state = GetIOSWindowSnapshot(window)) {
    if (width) *width = state->pixelWidth;
    if (height) *height = state->pixelHeight;
    return true;
  }
#endif
  return sdlMain<SDL_GetWindowSizeInPixels>(window, width, height);
}

inline SDL_WindowID windowID(SDL_Window *window) {
#if TARGET_OS_IPHONE
  if (const auto state = GetIOSWindowSnapshot(window)) return state->id;
#endif
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
