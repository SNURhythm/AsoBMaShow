#include "RAII.h"
#include <SDL3/SDL.h>
#include <bgfx/platform.h>
#include <bx/platform.h>

#include <cassert>
#include <cstring>
#include <type_traits>

// Keep dependency ABIs native to the host, then exercise the Android helper
// against the small ANativeWindow stub supplied by this test target.
#ifndef __ANDROID__
#define __ANDROID__ 1
#endif
#undef BX_PLATFORM_IOS
#undef BX_PLATFORM_OSX
#undef BX_PLATFORM_EMSCRIPTEN
#undef BX_PLATFORM_ANDROID
#define BX_PLATFORM_IOS 0
#define BX_PLATFORM_OSX 0
#define BX_PLATFORM_EMSCRIPTEN 0
#define BX_PLATFORM_ANDROID 1
#include "bgfx_helper.cpp"

struct ANativeWindow { int references = 1; };

namespace {
int windowToken;
ANativeWindow first, second;
ANativeWindow *publishedWindow = &first;
bool locked = false, failLock = false, missingProperties = false;
bool onMainThread = true;
int acquires = 0, releases = 0, lockCalls = 0;
}

extern "C" bool SDLCALL SDL_IsMainThread() { return onMainThread; }
extern "C" const char *SDLCALL SDL_GetError() { return "fixture dispatch failure"; }
extern "C" bool SDLCALL SDL_RunOnMainThread(SDL_MainThreadCallback callback, void *context, bool wait) {
  assert(wait && !onMainThread);
  onMainThread = true;
  callback(context);
  onMainThread = false;
  return true;
}
extern "C" SDL_PropertiesID SDLCALL SDL_GetWindowProperties(SDL_Window *window) {
  assert(onMainThread && window == reinterpret_cast<SDL_Window *>(&windowToken));
  return missingProperties ? 0 : 42;
}
extern "C" bool SDLCALL SDL_LockProperties(SDL_PropertiesID properties) {
  assert(properties == 42 && !locked);
  ++lockCalls;
  if (failLock) return false;
  locked = true;
  return true;
}
extern "C" void SDLCALL SDL_UnlockProperties(SDL_PropertiesID properties) {
  assert(properties == 42 && locked);
  locked = false;
}
extern "C" void *SDLCALL SDL_GetPointerProperty(SDL_PropertiesID properties,
                                               const char *name, void *fallback) {
  assert(properties == 42 && locked);
  assert(std::strcmp(name, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER) == 0);
  return publishedWindow ? publishedWindow : fallback;
}
extern "C" void ANativeWindow_acquire(ANativeWindow *window) {
  assert(locked && window && window->references > 0);
  ++window->references;
  ++acquires;
}
extern "C" void ANativeWindow_release(ANativeWindow *window) {
  assert(!locked && window && window->references > 0);
  --window->references;
  ++releases;
}

int main() {
  static_assert(!std::is_copy_constructible_v<AndroidNativeWindowOwner>);
  auto *window = reinterpret_cast<SDL_Window *>(&windowToken);
  assert(!acquireAndroidNativeWindow(nullptr) && lockCalls == 0);
  missingProperties = true;
  assert(!acquireAndroidNativeWindow(window) && lockCalls == 0);
  missingProperties = false;
  failLock = true;
  assert(!acquireAndroidNativeWindow(window) && !locked && acquires == 0);
  failLock = false;
  publishedWindow = nullptr;
  assert(!acquireAndroidNativeWindow(window) && !locked && acquires == 0);

  publishedWindow = &first;
  onMainThread = false; // The application worker must acquire through the SDL owner.
  auto current = acquireAndroidNativeWindow(window);
  onMainThread = true;
  assert(current.get() == &first && first.references == 2 && !locked);
  // SDL may release its reference after the render suspension handshake.
  publishedWindow = nullptr;
  ANativeWindow_release(&first);
  assert(first.references == 1);

  auto previous = std::move(current);
  publishedWindow = &second;
  current = acquireAndroidNativeWindow(window);
  assert(previous.get() == &first && current.get() == &second);
  assert(first.references == 1 && second.references == 2 && acquires == 2);
  // The old window survives until bgfx has processed the replacement frame.
  previous.reset();
  assert(first.references == 0 && second.references == 2);
  publishedWindow = nullptr;
  ANativeWindow_release(&second);
  assert(second.references == 1);
  current.reset();
  assert(second.references == 0 && releases == 4);
}
