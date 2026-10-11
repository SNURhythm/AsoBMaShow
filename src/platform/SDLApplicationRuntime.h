#pragma once

#include "../targets.h"
#if !TARGET_OS_IPHONE
#include <SDL3/SDL.h>
#include <cstdint>
#include <functional>
#include <optional>
#include "../ThreadCompat.h"

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
bool applicationCanPresent();
bool takeApplicationOverflow();
// Synchronous registration on the pump thread; clear before captured state dies.
void setApplicationEventDiscardHandler(std::function<void(const SDL_Event &)> handler);
void pollApplicationDiagnostics();
bool isApplicationThread();
// Queue renderer work at an owner-loop boundary. Native quit/background events
// request cancellation even while the owner is inside that work.
void postApplicationWork(std::function<void()> work, std::stop_source stop);
bool pollApplicationWork();
// Android may retire a surface without changing application focus.
void setApplicationSurfaceAvailable(bool available);
std::optional<WindowSnapshot> getWindowSnapshot(SDL_Window *window);
}
#endif
