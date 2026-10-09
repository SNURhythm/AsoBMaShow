#include "bgfx_helper.h"
#include <bx/platform.h>
#include <SDL3/SDL.h>
#if BX_PLATFORM_IOS || BX_PLATFORM_OSX
#include <SDL3/SDL_metal.h>
#endif

bool setup_bgfx_platform_data(bgfx::PlatformData &pd, SDL_Window *sdlWindow,
                              SdlMetalViewOwner &metalView) {
  pd = {};
#if BX_PLATFORM_IOS || BX_PLATFORM_OSX
  SdlMetalViewOwner created(SDL_Metal_CreateView(sdlWindow));
  if (!created) return false;
  pd.nwh = SDL_Metal_GetLayer(created.get());
  if (pd.nwh == nullptr) return false;
  metalView = std::move(created);
#elif BX_PLATFORM_EMSCRIPTEN
  (void)metalView;
  pd.nwh = (void *)"#canvas";
#else
  (void)metalView;
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
  } else if (driver != nullptr && SDL_strcmp(driver, "vivante") == 0) {
    pd.ndt = SDL_GetPointerProperty(properties,
        SDL_PROP_WINDOW_VIVANTE_DISPLAY_POINTER, nullptr);
    pd.nwh = SDL_GetPointerProperty(properties,
        SDL_PROP_WINDOW_VIVANTE_WINDOW_POINTER, nullptr);
  }
#endif
#endif
  return pd.nwh != nullptr;
}
