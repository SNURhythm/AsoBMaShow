#pragma once
#include "RAII.h"
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_metal.h>
#include <bgfx/bgfx.h>

using SdlMetalViewOwner = UniqueResource<void, SDL_Metal_DestroyView>;

#if defined(__ANDROID__)
#include <android/native_window.h>
using AndroidNativeWindowOwner =
    UniqueResource<ANativeWindow, ANativeWindow_release>;

// Retain under SDL's property lock before its surface callback can release it.
// Keep the owner alive until bgfx has retired all uses of this native window.
AndroidNativeWindowOwner acquireAndroidNativeWindow(SDL_Window *window);
#endif

// Keep the Metal view alive until bgfx shuts down, then reset it before the window.
bool setup_bgfx_platform_data(bgfx::PlatformData &pd, SDL_Window *sdlWindow,
                              SdlMetalViewOwner &metalView);

// Main-thread surface replacement. Keep retired alive until the renderer has
// consumed the new platform data, then release it on main.
bool replace_bgfx_metal_view(bgfx::PlatformData &pd, SDL_Window *window,
                            SdlMetalViewOwner &current, SdlMetalViewOwner &retired);
