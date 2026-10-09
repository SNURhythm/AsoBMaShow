#include "bgfx_helper.h"
#include <bx/platform.h>
#include <SDL3/SDL.h>
#if BX_PLATFORM_IOS || BX_PLATFORM_OSX
#include <SDL3/SDL_metal.h>
#endif

bool setup_bgfx_platform_data(bgfx::PlatformData &pd, SDL_Window *sdlWindow) {
  pd = {};
#if BX_PLATFORM_IOS || BX_PLATFORM_OSX
  SDL_MetalView metalView = SDL_Metal_CreateView(sdlWindow);
  if (metalView == nullptr) return false;
  pd.nwh = SDL_Metal_GetLayer(metalView);
#elif BX_PLATFORM_EMSCRIPTEN
  pd.nwh = (void *)"#canvas";
#else
  const SDL_PropertiesID properties = SDL_GetWindowProperties(sdlWindow);
#if BX_PLATFORM_ANDROID
  pd.nwh = SDL_GetPointerProperty(properties,
      SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
#elif BX_PLATFORM_WINDOWS
  pd.nwh = SDL_GetPointerProperty(properties,
      SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
  const char *driver = SDL_GetCurrentVideoDriver();
  if (driver != nullptr && SDL_strcmp(driver, "x11") == 0) {
    pd.ndt = SDL_GetPointerProperty(properties,
        SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    pd.nwh = reinterpret_cast<void *>(static_cast<uintptr_t>(
        SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
  } else if (driver != nullptr && SDL_strcmp(driver, "wayland") == 0) {
    pd.ndt = SDL_GetPointerProperty(properties,
        SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    pd.nwh = SDL_GetPointerProperty(properties,
        SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    pd.type = bgfx::NativeWindowHandleType::Wayland;
  }
#endif
#endif
  return pd.nwh != nullptr;
}
