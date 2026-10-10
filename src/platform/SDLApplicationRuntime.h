#pragma once

#include "../targets.h"
#if !TARGET_OS_IPHONE
#include <SDL3/SDL.h>
#include <cstdint>
#include <functional>
#include <optional>

namespace platform {
struct WindowSnapshot {
  SDL_Window *window = nullptr;
  SDL_WindowID id = 0;
  int width = 0, height = 0;
  int pixelWidth = 0, pixelHeight = 0;
  float refreshRate = 0;
  std::uint64_t generation = 0;
};

// SDL's bootstrap thread owns events/windows until worker cleanup completes.
// On Android this is SDLThread, not Java's UI thread.
int runSDLApplication(SDL_Window *window, std::function<int()> application);
bool pollApplicationEvent(SDL_Event *event);
bool waitApplicationEvent(SDL_Event *event, int timeoutMs);
bool applicationActive();
bool takeApplicationOverflow();
void pollApplicationDiagnostics();
std::optional<WindowSnapshot> getWindowSnapshot(SDL_Window *window);
}
#endif
