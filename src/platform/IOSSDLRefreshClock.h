#pragma once
#include "IOSApplicationRuntime.h"

#if TARGET_OS_IPHONE
// SDL installs a no-op CADisplayLink on >60 Hz displays even without an
// animation callback. The Metal link already requests the rendering rate;
// keep SDL's high-refresh request only while timer fallback needs it.
inline void SetIOSSDLRefreshClockEnabled(SDL_Window *window, bool enabled) {
  platform::runOnMain([window, enabled] {
    const auto *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    SDL_iOSAnimationCallback callback = nullptr;
    if (enabled && mode != nullptr && mode->refresh_rate > 60.0F) {
      callback = [](void *) {}; // Refresh request only; never run application work here.
    }
    if (!SDL_SetiOSAnimationCallback(window, 1, callback, nullptr)) {
      SDL_Log("Could not update SDL fallback refresh clock: %s", SDL_GetError());
    }
  });
}
#endif
