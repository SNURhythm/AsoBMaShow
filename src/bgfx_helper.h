#pragma once
#include "RAII.h"
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_metal.h>
#include <bgfx/platform.h>

using SdlMetalViewOwner = UniqueResource<void, SDL_Metal_DestroyView>;

// Keep the Metal view alive until bgfx shuts down, then reset it before the window.
bool setup_bgfx_platform_data(bgfx::PlatformData &pd, SDL_Window *sdlWindow,
                              SdlMetalViewOwner &metalView);
