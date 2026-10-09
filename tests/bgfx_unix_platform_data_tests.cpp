#include "bgfx_helper.h"
#include <SDL3/SDL.h>
#include <bx/platform.h>

#include <cassert>
#include <cstring>

// Exercise the Unix driver dispatch on every desktop test host. Load dependency
// headers first so their ABI still uses the actual host platform.
#undef BX_PLATFORM_IOS
#undef BX_PLATFORM_OSX
#undef BX_PLATFORM_EMSCRIPTEN
#undef BX_PLATFORM_ANDROID
#undef BX_PLATFORM_WINDOWS
#define BX_PLATFORM_IOS 0
#define BX_PLATFORM_OSX 0
#define BX_PLATFORM_EMSCRIPTEN 0
#define BX_PLATFORM_ANDROID 0
#define BX_PLATFORM_WINDOWS 0
#include "bgfx_helper.cpp"

namespace {
int windowToken, displayToken, surfaceToken;
const char *driver;
const char *displayProperty;
const char *windowProperty;
bool missingWindow = false;
}

extern "C" SDL_PropertiesID SDLCALL SDL_GetWindowProperties(SDL_Window *window) {
  assert(window == reinterpret_cast<SDL_Window *>(&windowToken));
  return 42;
}

extern "C" const char *SDLCALL SDL_GetCurrentVideoDriver() { return driver; }
extern "C" int SDLCALL SDL_strcmp(const char *left, const char *right) {
  return std::strcmp(left, right);
}
extern "C" void *SDLCALL SDL_GetPointerProperty(SDL_PropertiesID properties,
                                               const char *name, void *fallback) {
  assert(properties == 42);
  if (std::strcmp(name, displayProperty) == 0) return &displayToken;
  if (std::strcmp(name, windowProperty) == 0 && !missingWindow) return &surfaceToken;
  return fallback;
}
extern "C" Sint64 SDLCALL SDL_GetNumberProperty(SDL_PropertiesID properties,
                                              const char *name, Sint64 fallback) {
  assert(properties == 42);
  if (std::strcmp(name, SDL_PROP_WINDOW_X11_WINDOW_NUMBER) == 0 && !missingWindow)
    return 123;
  return fallback;
}
extern "C" void SDLCALL SDL_Metal_DestroyView(SDL_MetalView) { assert(false); }

int main() {
  auto *window = reinterpret_cast<SDL_Window *>(&windowToken);
  SdlMetalViewOwner owner;
  bgfx::PlatformData data{};
  struct Fixture {
    const char *driver;
    const char *display;
    const char *window;
    void *handle;
    bgfx::NativeWindowHandleType::Enum type;
  };
  const Fixture fixtures[] = {
      {"x11", SDL_PROP_WINDOW_X11_DISPLAY_POINTER, "unused",
       reinterpret_cast<void *>(123), bgfx::NativeWindowHandleType::Default},
      {"wayland", SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER,
       SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, &surfaceToken,
       bgfx::NativeWindowHandleType::Wayland},
      {"vivante", SDL_PROP_WINDOW_VIVANTE_DISPLAY_POINTER,
       SDL_PROP_WINDOW_VIVANTE_WINDOW_POINTER, &surfaceToken,
       bgfx::NativeWindowHandleType::Default},
  };
  for (const auto &fixture : fixtures) {
    driver = fixture.driver;
    displayProperty = fixture.display;
    windowProperty = fixture.window;
    missingWindow = false;
    assert(setup_bgfx_platform_data(data, window, owner));
    assert(data.ndt == &displayToken && data.nwh == fixture.handle);
    assert(data.type == fixture.type && !owner);
    assert(!data.context && !data.backBuffer && !data.backBufferDS);
    missingWindow = true;
    assert(!setup_bgfx_platform_data(data, window, owner));
    assert(data.nwh == nullptr);
  }
  for (const char *unsupported : {"dummy", static_cast<const char *>(nullptr)}) {
    driver = unsupported;
    assert(!setup_bgfx_platform_data(data, window, owner));
    assert(data.ndt == nullptr && data.nwh == nullptr);
  }
}
